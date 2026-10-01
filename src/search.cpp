// search.cpp - `rbsim optimize`: improve an existing TAS by local search.
//
// A route is the per-frame input list of one level attempt (no R). Its score is the frame on
// which the flag is collected (Sim::winFrame), counting a death-warp finish only if it is
// usable in a real run (DeathWarpFinishValid). Lower is better.
//
// Each worker thread repeatedly applies a random edit to the current best route (flip one
// frame, move a run boundary, delete/insert a frame, overwrite a short block, or two of these)
// and replays it from a cached snapshot of the best route taken just before the first changed
// frame. A replay stops as soon as it can no longer match the best. Strictly better routes
// replace the best; equally fast ones replace it with probability `--sideways`, so the search
// can drift across plateaus. Workers share the best through a mutex and rebuild their
// snapshot caches when it changes.
#include "redball.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace rb;

namespace {

constexpr int NO_WIN = 1 << 30;
constexpr int SNAP_EVERY = 8;  // frames between cached snapshots of the best route
std::atomic<uint64_t> g_ticks{0};

struct Result {
    int win = NO_WIN;      // flag frame (score)
    int death = -1;        // death frame if the finish is a death warp
    int uncertain = 0;     // Sim::displayUncertain at the finish (route should be checked in Flash)
    double margin = 0;     // overlap depth with the flag at the win (twips): tie-breaker, deeper is better
};
// a strictly better than b: earlier flag, or same frame with a deeper overlap
static bool Better(const Result& a, const Result& b) { return a.win < b.win || (a.win == b.win && a.margin > b.margin); }

// Continue `sim` (already advanced by `start` ticks) with route[start..]; inputs past the end of
// the route are IN_NONE. Gives up once the flag can no longer come by frame `limit`.
Result Finish(Sim& sim, const std::vector<uint8_t>& route, int start, int limit) {
    Result r;
    int t = start;
    struct Count {
        int& t;
        int s;
        ~Count() { g_ticks.fetch_add((uint64_t)(t - s), std::memory_order_relaxed); }
    } count{t, start};
    for (; t < limit; ++t) {
        if (!sim.playerAlive && sim.deathFrame >= 0 && sim.frameCount >= sim.deathFrame + RESPAWN_LAST_PAUSE_FRAMES) break;
        sim.Tick(t < (int)route.size() ? route[(size_t)t] : (uint8_t)IN_NONE);
        if (sim.isTimeStop) {
            if (!DeathWarpFinishValid(sim.deathFrame, sim.winFrame)) break;
            r.win = sim.winFrame;
            r.death = sim.deathFrame;
            r.uncertain = sim.displayUncertain;
            r.margin = sim.winMargin;
            break;
        }
    }
    return r;
}

struct Shared {
    std::mutex m;
    std::vector<uint8_t> best;
    Result bestRes;
    uint64_t version = 0;
    std::atomic<uint64_t> evals{0}, improvements{0}, sideways{0};
};

struct Worker {
    LevelTemplate* tpl;
    int checkpoint;
    bool gless;
    std::vector<std::unique_ptr<Sim>> snaps;  // snaps[j]: state after j * SNAP_EVERY ticks of `route`
    std::vector<uint8_t> route;
    Result res;
    uint64_t version = ~0ULL;

    void Rebuild() {
        snaps.clear();
        auto s = std::make_unique<Sim>();
        s->Load(tpl, checkpoint);
        s->gless = gless;
        for (int t = 0;; ++t) {
            if (t % SNAP_EVERY == 0) snaps.push_back(std::make_unique<Sim>(*s));
            if (t >= res.win) break;
            s->Tick(t < (int)route.size() ? route[(size_t)t] : (uint8_t)IN_NONE);
        }
    }
};

// Run boundaries of a route: indices t > 0 with route[t] != route[t - 1].
static void Boundaries(const std::vector<uint8_t>& r, std::vector<int>& out) {
    out.clear();
    for (size_t t = 1; t < r.size(); ++t)
        if (r[t] != r[t - 1]) out.push_back((int)t);
}

// One random edit; returns the first changed frame (or -1 if nothing changed).
static int Mutate(std::vector<uint8_t>& r, std::mt19937_64& rng, std::vector<int>& bounds) {
    const int L = (int)r.size();
    if (L == 0) return -1;
    auto rnd = [&](int n) { return (int)(rng() % (uint64_t)n); };
    switch (rnd(6)) {
        case 0: {  // flip one frame
            const int t = rnd(L);
            const uint8_t c = (uint8_t)((r[(size_t)t] + 1 + rnd(7)) % 8);
            r[(size_t)t] = c;
            return t;
        }
        case 1:
        case 2: {  // move a run boundary by 1..3 frames
            Boundaries(r, bounds);
            if (bounds.empty()) return -1;
            const int b = bounds[(size_t)rnd((int)bounds.size())];
            const int k = 1 + rnd(3);
            if (rnd(2)) {  // left run grows into the right one
                for (int t = b; t < std::min(L, b + k); ++t) r[(size_t)t] = r[(size_t)(b - 1)];
                return b;
            }
            const int from = std::max(0, b - k);  // right run grows into the left one
            for (int t = from; t < b; ++t) r[(size_t)t] = r[(size_t)b];
            return from;
        }
        case 3: {  // delete a frame
            const int t = rnd(L);
            r.erase(r.begin() + t);
            return t;
        }
        case 4: {  // insert a frame (duplicate of a neighbour or random)
            const int t = rnd(L);
            const uint8_t c = rnd(2) ? r[(size_t)t] : (uint8_t)rnd(8);
            r.insert(r.begin() + t, c);
            return t;
        }
        default: {  // overwrite a block of 2..8 frames with one input
            const int len = 2 + rnd(7);
            const int t = rnd(L);
            const uint8_t c = (uint8_t)rnd(8);
            for (int u = t; u < std::min(L, t + len); ++u) r[(size_t)u] = c;
            return t;
        }
    }
}

void WorkerLoop(Shared& sh, Worker w, uint64_t seed, double sidewaysP, std::chrono::steady_clock::time_point deadline,
                bool verbose, uint64_t maxEvals) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> uni(0, 1);
    std::vector<int> bounds;
    std::vector<uint8_t> cand;
    Sim sim;
    while (std::chrono::steady_clock::now() < deadline && (!maxEvals || sh.evals.load() < maxEvals)) {
        {
            std::lock_guard<std::mutex> lk(sh.m);
            if (w.version != sh.version) {
                w.route = sh.best;
                w.res = sh.bestRes;
                w.version = sh.version;
                w.Rebuild();
            }
        }
        for (int batch = 0; batch < 64; ++batch) {
            cand = w.route;
            int first = Mutate(cand, rng, bounds);
            if (rng() % 3 == 0) {  // sometimes a second edit
                const int f2 = Mutate(cand, rng, bounds);
                if (f2 >= 0) first = first < 0 ? f2 : std::min(first, f2);
            }
            if (first < 0 || cand == w.route) continue;
            if (maxEvals && sh.evals.load() >= maxEvals) break;
            const int j = std::min(first / SNAP_EVERY, (int)w.snaps.size() - 1);
            sim = *w.snaps[(size_t)j];
            const Result r = Finish(sim, cand, j * SNAP_EVERY, w.res.win + 1);
            sh.evals.fetch_add(1, std::memory_order_relaxed);
            if (r.win > w.res.win) continue;
            const bool better = Better(r, w.res);
            if (!better && (r.win != w.res.win || r.margin < w.res.margin || uni(rng) >= sidewaysP)) continue;
            cand.resize((size_t)std::min((int)cand.size(), r.win));  // inputs after the flag are irrelevant
            std::lock_guard<std::mutex> lk(sh.m);
            if (Better(sh.bestRes, r)) break;                  // someone else got further meanwhile
            if (!better && sh.version != w.version) break;     // stale sideways move
            sh.best = cand;
            sh.bestRes = r;
            ++sh.version;
            if (better) {
                sh.improvements.fetch_add(1);
                if (verbose)
                    std::printf("  frame %d, margin %.0f twips%s  %s\n", r.win, r.margin, r.death >= 0 ? " (death warp)" : "",
                                EncodeInputsRLE(cand).c_str());
                std::fflush(stdout);
            } else {
                sh.sideways.fetch_add(1);
            }
            break;
        }
    }
}

}  // namespace

int CmdOptimize(int argc, char** argv) {
    int level = 0, checkpoint = 0, threads = (int)std::max(1u, std::thread::hardware_concurrency());
    double seconds = 30, sidewaysP = 0.05;
    uint64_t seed = 1, maxEvals = 0;
    bool gless = false, quiet = false;
    std::string inputs;
    for (int i = 2; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--level")) level = std::atoi(next());
        else if (!std::strcmp(argv[i], "--checkpoint")) checkpoint = std::atoi(next());
        else if (!std::strcmp(argv[i], "--inputs")) inputs = next();
        else if (!std::strcmp(argv[i], "--time")) seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--threads")) threads = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--sideways")) sidewaysP = std::atof(next());
        else if (!std::strcmp(argv[i], "--evals")) {
            maxEvals = std::strtoull(next(), nullptr, 10);
            seconds = 1e9;  // stop on the count only
        }
        else if (!std::strcmp(argv[i], "--gless")) gless = true;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!level || inputs.empty()) {
        std::fprintf(stderr,
                     "usage: rbsim optimize --level N --inputs RLE [--checkpoint C] [--time SEC] [--threads N]\n"
                     "                      [--seed S] [--sideways P] [--evals N] [--gless] [--quiet]\n"
                     "  --evals N stops after N candidates (with --threads 1: identical results on any machine)\n");
        return 2;
    }
    std::vector<uint8_t> route = DecodeInputs(inputs);
    for (uint8_t c : route)
        if (c > 7) {
            std::fprintf(stderr, "optimize works on one attempt: remove R from the input string\n");
            return 2;
        }
    LevelTemplate tpl(level);
    Shared sh;
    {
        auto s = std::make_unique<Sim>();
        s->Load(&tpl, checkpoint);
        s->gless = gless;
        sh.bestRes = Finish(*s, route, 0, (int)route.size() + 200);
    }
    if (sh.bestRes.win == NO_WIN) {
        std::fprintf(stderr, "the given inputs do not collect the flag (or only via an unusable death warp)\n");
        return 1;
    }
    route.resize((size_t)std::min((int)route.size(), sh.bestRes.win));
    sh.best = route;
    const int startWin = sh.bestRes.win;
    if (maxEvals)
        std::printf("level %d, start: frame %d%s (%.3f s), flag overlap %.0f twips, %d threads, %llu candidates\n", level, startWin,
                    sh.bestRes.death >= 0 ? " (death warp)" : "", startWin / 31.0, sh.bestRes.margin, threads, (unsigned long long)maxEvals);
    else
        std::printf("level %d, start: frame %d%s (%.3f s), flag overlap %.0f twips, %d threads, %.0f s\n", level, startWin,
                    sh.bestRes.death >= 0 ? " (death warp)" : "", startWin / 31.0, sh.bestRes.margin, threads, seconds);
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds((long long)std::min(seconds * 1000, 1e15));
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k) {
        Worker w{&tpl, checkpoint, gless, {}, {}, {}, ~0ULL};
        pool.emplace_back(WorkerLoop, std::ref(sh), std::move(w), seed * 1000003ULL + (uint64_t)k, sidewaysP, deadline, !quiet, maxEvals);
    }
    for (auto& t : pool) t.join();
    const Result& b = sh.bestRes;
    std::printf("evaluated %llu candidates (%llu improvements, %llu sideways moves), %.0f frames simulated per second, %.0f per candidate\n",
                (unsigned long long)sh.evals.load(), (unsigned long long)sh.improvements.load(), (unsigned long long)sh.sideways.load(),
                g_ticks.load() / std::max(1e-9, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()), sh.evals.load() ? (double)g_ticks.load() / (double)sh.evals.load() : 0.0);
    std::printf("best: frame %d%s (%.3f s), %d frame%s saved, flag overlap %.0f twips\n", b.win, b.death >= 0 ? " (death warp)" : "",
                b.win / 31.0, startWin - b.win, startWin - b.win == 1 ? "" : "s", b.margin);
    if (b.death >= 0)
        std::printf("death warp: death on frame %d, flag on frame %d; pause by frame %d, unpause on frame %d\n", b.death, b.win,
                    b.death + RESPAWN_LAST_PAUSE_FRAMES, b.win + WIN_TIMER_FRAMES);
    if (b.uncertain)
        std::printf("note: %d display test%s within one matrix unit of flipping: check this route in Flash\n", b.uncertain,
                    b.uncertain == 1 ? "" : "s");
    std::printf("%s\n", EncodeInputsRLE(sh.best).c_str());
    return 0;
}
