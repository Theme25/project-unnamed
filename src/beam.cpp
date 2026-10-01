// beam.cpp - `rbsim beam`: breadth-first beam search for a fast route from any start state.
//
// Layer k holds up to W states reached after k frames from the start (level start, a
// checkpoint, or the end of a given input prefix). Each frame:
//   1. score pass (parallel): every state tries all 8 inputs. A child that collects the flag is
//      a solution. A child that dies is played through its death-warp window at once (dead
//      states ignore inputs) and kept only as a solution if it warps to the flag in time for a
//      real run (DeathWarpFinishValid). Living children get a score (navigation distance to the
//      flag, lower is better) and a 64-bit hash of their compact encoding.
//   2. select: candidates are sorted by (score, hash, parent, input), exact duplicates are
//      dropped, at most --diversity children per region bucket are kept, up to W in total.
//      The order does not depend on the thread count, so results are reproducible.
//   3. store pass (parallel): the selected children are re-simulated and stored as compact
//      snapshots (src/snapshot.h).
// Parent links of every layer go to <dir>/links/NNNNNN.bin so the input string of any solution
// can be rebuilt; the beam itself is saved to <dir>/beam.bin every --save-every minutes and at
// the end, so --resume continues an interrupted run.
//
// The search stops when no child can reach the flag before the best solution found so far, at
// --max-frames, or when the beam empties. A beam search is a heuristic: it finds strong routes,
// it does not prove that no faster route exists.
#include "redball.h"
#include "snapshot.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace rb;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- navigation distance field
// Grid over the level (cell = 10 px). A cell is blocked if its centre lies inside a static
// shape. Distances (px) are 8-neighbour Dijkstra distances to the cells whose centre is within
// the ball radius of the flag's box. Moving bodies and spikes are ignored.
struct NavField {
    double x0 = 0, y0 = 0, cell = 10;
    int nx = 0, ny = 0;
    std::vector<float> dist;
    static constexpr float INF = 1e30f;

    void Build(const Sim& s) {
        const World& w = s.world;
        double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
        auto grow = [&](double x, double y) {
            minx = std::min(minx, x), miny = std::min(miny, y), maxx = std::max(maxx, x), maxy = std::max(maxy, y);
        };
        std::vector<int32_t> stat;
        for (int32_t sh = 0; sh < w.numShapes; ++sh) {
            const int32_t b = w.shapes[sh].body;
            if (b < 0 || !w.bodies[b].inWorld || !w.bodies[b].IsStatic()) continue;
            AABB box;
            ComputeAABB(w.geoms->geoms[(size_t)w.shapes[sh].geom], box, w.bodies[b].xf);
            grow(box.lowerBound.x * PHYS_SCALE, box.lowerBound.y * PHYS_SCALE);
            grow(box.upperBound.x * PHYS_SCALE, box.upperBound.y * PHYS_SCALE);
            stat.push_back(sh);
        }
        const DisplayObj* aim = s.tpl->aim;
        if (aim) grow(aim->x0 / 20, aim->y0 / 20), grow(aim->x1 / 20, aim->y1 / 20);
        grow(s.spriteX[s.playerBody], s.spriteY[s.playerBody]);
        minx -= 300, miny -= 300, maxx += 300, maxy += 300;
        cell = 10;
        while ((maxx - minx) / cell * (maxy - miny) / cell > 4e6) cell *= 1.5;
        x0 = minx, y0 = miny;
        nx = (int)std::ceil((maxx - minx) / cell), ny = (int)std::ceil((maxy - miny) / cell);
        std::vector<uint8_t> blocked((size_t)nx * ny, 0);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const Vec2 p((x0 + (i + 0.5) * cell) / PHYS_SCALE, (y0 + (j + 0.5) * cell) / PHYS_SCALE);
                for (int32_t sh : stat)
                    if (w.ShapeTestPoint(sh, w.bodies[w.shapes[sh].body].xf, p)) {
                        blocked[(size_t)j * nx + i] = 1;
                        break;
                    }
            }
        dist.assign((size_t)nx * ny, INF);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        if (aim) {
            const double r = 10.5;
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    const double cx = x0 + (i + 0.5) * cell, cy = y0 + (j + 0.5) * cell;
                    if (cx >= aim->x0 / 20 - r && cx <= aim->x1 / 20 + r && cy >= aim->y0 / 20 - r && cy <= aim->y1 / 20 + r) {
                        dist[(size_t)j * nx + i] = 0;
                        q.push({0.f, j * nx + i});
                    }
                }
        }
        const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        while (!q.empty()) {
            const QE e = q.top();
            q.pop();
            if (e.first > dist[(size_t)e.second]) continue;
            const int i = e.second % nx, j = e.second / nx;
            for (int k = 0; k < 8; ++k) {
                const int a = i + di[k], b = j + dj[k];
                if (a < 0 || b < 0 || a >= nx || b >= ny || blocked[(size_t)b * nx + a]) continue;
                const float nd = e.first + (float)(cell * (k < 4 ? 1.0 : 1.41421356));
                if (nd < dist[(size_t)b * nx + a]) {
                    dist[(size_t)b * nx + a] = nd;
                    q.push({nd, b * nx + a});
                }
            }
        }
    }
    double Cell(int i, int j) const {  // distance at a cell, or the nearest reachable cell's (+ offset)
        const int ci = std::min(std::max(i, 0), nx - 1), cj = std::min(std::max(j, 0), ny - 1);
        const double ox = (double)(i - ci) * cell, oy = (double)(j - cj) * cell;
        double extra = std::sqrt(ox * ox + oy * oy);  // sqrt is exact on every platform; hypot is not
        float d = dist[(size_t)cj * nx + ci];
        for (int r = 1; d >= INF && r < 6; ++r)
            for (int b = cj - r; b <= cj + r; ++b)
                for (int a = ci - r; a <= ci + r; ++a)
                    if (a >= 0 && b >= 0 && a < nx && b < ny && dist[(size_t)b * nx + a] < d) {
                        d = dist[(size_t)b * nx + a];
                        extra = std::max(extra, r * cell);
                    }
        return d >= INF ? 1e9 : d + extra;
    }
    // smooth distance (px) at a point: bilinear interpolation between cell centres
    double Query(double px, double py) const {
        const double fx = (px - x0) / cell - 0.5, fy = (py - y0) / cell - 0.5;
        const int i = (int)std::floor(fx), j = (int)std::floor(fy);
        const double tx = fx - i, ty = fy - j;
        const double a = Cell(i, j), b = Cell(i + 1, j), c = Cell(i, j + 1), d = Cell(i + 1, j + 1);
        if (a >= 1e9 || b >= 1e9 || c >= 1e9 || d >= 1e9) return std::min(std::min(a, b), std::min(c, d));
        return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
    }
};

// ---------------------------------------------------------------- helpers
uint64_t Hash64(const uint8_t* p, size_t n) {  // FNV-1a over 8-byte words + final mix
    uint64_t h = 1469598103934665603ULL ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ULL;
        h ^= h >> 31;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

struct Cand {
    double score;
    uint64_t hash;
    uint32_t parent;
    uint8_t input;
    uint64_t bucket;
};

struct Solution {
    int win = 1 << 30;     // flag frame (Sim::winFrame)
    double margin = 0;     // flag overlap (twips), tie-breaker
    int death = -1;
    int layer = -1;        // layer of the parent state
    uint32_t parent = 0;   // index of the parent in that layer
    uint8_t input = 0;     // input of the winning (or dying) frame
    int deadFrames = 0;    // frames played after death (inputs irrelevant: written as n)
    // total order, so the reported solution does not depend on which thread found it first
    bool Better(const Solution& o) const {
        if (win != o.win) return win < o.win;
        if (margin != o.margin) return margin > o.margin;
        if (layer != o.layer) return layer < o.layer;
        if (parent != o.parent) return parent < o.parent;
        if (input != o.input) return input < o.input;
        return deadFrames < o.deadFrames;
    }
};

template <class F>
void Parallel(int threads, size_t n, F f) {
    std::atomic<size_t> next{0};
    auto work = [&](int tid) {
        for (;;) {
            const size_t i = next.fetch_add(64);
            if (i >= n) break;
            for (size_t k = i; k < std::min(n, i + 64); ++k) f(tid, k);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& t : pool) t.join();
}

size_t ParseSize(const char* s) {  // "10G", "512M", bytes
    double v = std::atof(s);
    const char u = s[std::strlen(s) - 1];
    if (u == 'G' || u == 'g') v *= 1024.0 * 1024 * 1024;
    else if (u == 'M' || u == 'm') v *= 1024.0 * 1024;
    return (size_t)v;
}

const uint32_t MAGIC = 0x52424D31;  // "RBM1"

}  // namespace

int CmdBeam(int argc, char** argv) {
    int level = 0, checkpoint = 0, threads = (int)std::max(1u, std::thread::hardware_concurrency());
    int maxFrames = 3000, diversity = 16;
    double lookahead = 6;
    size_t memory = 0, fixedWidth = 0;
    double saveMinutes = 10;
    bool gless = false, resume = false;
    std::string prefixStr, dir, explainStr, seedStr;
    int selectMode = 2;  // 0 score, 1 coverage, 2 mixed (default)
    for (int i = 2; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--level")) level = std::atoi(next());
        else if (!std::strcmp(argv[i], "--checkpoint")) checkpoint = std::atoi(next());
        else if (!std::strcmp(argv[i], "--prefix")) prefixStr = next();
        else if (!std::strcmp(argv[i], "--memory")) memory = ParseSize(next());
        else if (!std::strcmp(argv[i], "--width")) fixedWidth = (size_t)std::atoll(next());
        else if (!std::strcmp(argv[i], "--threads")) threads = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--max-frames")) maxFrames = std::atoi(next());
        else if (!std::strcmp(argv[i], "--diversity")) diversity = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--lookahead")) lookahead = std::atof(next());
        else if (!std::strcmp(argv[i], "--dir")) dir = next();
        else if (!std::strcmp(argv[i], "--save-every")) saveMinutes = std::atof(next());
        else if (!std::strcmp(argv[i], "--resume")) resume = true;
        else if (!std::strcmp(argv[i], "--explain")) explainStr = next();
        else if (!std::strcmp(argv[i], "--seed-route")) seedStr = next();
        else if (!std::strcmp(argv[i], "--select")) {
            const char* m = next();
            selectMode = !std::strcmp(m, "score") ? 0 : !std::strcmp(m, "coverage") ? 1 : 2;
        }
        else if (!std::strcmp(argv[i], "--gless")) gless = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!level) {
        std::fprintf(stderr,
                     "usage: rbsim beam --level N [--checkpoint C] [--prefix RLE] [--memory 10G | --width W]\n"
                     "                  [--threads N] [--max-frames F] [--diversity K] [--lookahead T] [--dir DIR]\n"
                     "                  [--save-every MIN] [--resume] [--gless] [--select mixed|score|coverage]\n"
                     "                  [--seed-route RLE] [--explain RLE]\n");
        return 2;
    }
    if (dir.empty()) dir = "beam_L" + std::to_string(level) + (checkpoint ? "_cp" + std::to_string(checkpoint) : "");
    if (!memory && !fixedWidth) memory = (size_t)4 << 30;  // conservative default: 4 GB

    LevelTemplate tpl(level);
    auto base = std::make_unique<Sim>();
    base->Load(&tpl, checkpoint);
    base->gless = gless;
    const std::vector<uint8_t> prefix = DecodeInputs(prefixStr);
    for (uint8_t c : prefix)
        if (c > 7) {
            std::fprintf(stderr, "--prefix must not contain R\n");
            return 2;
        }

    // ---- start state (or resume)
    std::vector<std::vector<uint8_t>> layer;  // compact snapshots of the current layer
    int k = 0;                                // current layer index (frames after the prefix)
    Solution best;
    fs::create_directories(fs::path(dir) / "links");
    const fs::path stateFile = fs::path(dir) / "beam.bin";
    if (resume) {
        std::ifstream in(stateFile, std::ios::binary);
        uint32_t magic = 0, n = 0;
        int lv = 0, cp = 0, gl = 0;
        uint32_t plen = 0;
        in.read((char*)&magic, 4).read((char*)&lv, 4).read((char*)&cp, 4).read((char*)&gl, 4).read((char*)&plen, 4);
        std::string pre(plen, '\0');
        in.read(&pre[0], plen);
        in.read((char*)&k, 4).read((char*)&best, sizeof best).read((char*)&n, 4);
        if (!in || magic != MAGIC || lv != level || cp != checkpoint || (gl != 0) != gless || pre != prefixStr) {
            std::fprintf(stderr, "cannot resume: %s missing or made with different --level/--checkpoint/--prefix/--gless\n",
                         stateFile.string().c_str());
            return 1;
        }
        layer.resize(n);
        for (auto& e : layer) {
            uint32_t len = 0;
            in.read((char*)&len, 4);
            e.resize(len);
            in.read((char*)e.data(), len);
        }
        if (!in) {
            std::fprintf(stderr, "cannot resume: %s is truncated\n", stateFile.string().c_str());
            return 1;
        }
        std::printf("resumed %s at frame %d of the search (%zu states)\n", dir.c_str(), k, layer.size());
    } else {
        auto s = std::make_unique<Sim>(*base);
        for (uint8_t c : prefix) {
            s->Tick(c);
            if (!s->playerAlive || s->isTimeStop) {
                std::fprintf(stderr, "the prefix already ends the attempt (death or flag)\n");
                return 1;
            }
        }
        layer.emplace_back();
        EncodeSnapshot(*base, *s, layer.back());
    }
    const int startFrame = (int)prefix.size();  // frameCount of layer 0

    // ---- navigation field, always built from the start state (level/checkpoint + prefix), never from a
    // resumed layer: the grid origin depends on the ball position, and scores must not change on resume
    NavField nav;
    auto startState = std::make_unique<Sim>(*base);
    for (uint8_t c : prefix) startState->Tick(c);
    nav.Build(*startState);
    std::printf("level %d%s, start frame %d, nav grid %dx%d (%.0f px cells), %d threads, %s\n", level,
                checkpoint ? (" checkpoint " + std::to_string(checkpoint)).c_str() : "", startFrame, nav.nx, nav.ny, nav.cell, threads,
                fixedWidth ? ("width " + std::to_string(fixedWidth)).c_str()
                           : ("memory " + std::to_string(memory >> 20) + " MB").c_str());

    // --explain RLE: print the score the beam would give a known route at each frame, to compare with
    // the beam's best scores (diagnoses why a beam misses a route), then exit.
    if (!explainStr.empty()) {
        auto s = std::make_unique<Sim>(*startState);
        const std::vector<uint8_t> r = DecodeInputs(explainStr);
        for (size_t f = 0; f < r.size(); ++f) {
            s->Tick(r[f]);
            if (s->isTimeStop || !s->playerAlive) {
                std::printf("frame %5d  %s\n", s->frameCount, s->isTimeStop ? "flag" : "death");
                break;
            }
            const Body& pb = s->world.bodies[s->playerBody];
            const double sc = nav.Query(s->spriteX[s->playerBody] + pb.linearVelocity.x * lookahead,
                                        s->spriteY[s->playerBody] + pb.linearVelocity.y * lookahead);
            if ((f + 1) % 10 == 0) std::printf("frame %5d  score %7.1f px\n", s->frameCount, sc);
        }
        return 0;
    }

    // --seed-route: a known full route (from the level start / checkpoint; it must begin with --prefix).
    // seedHash[j] / seedScore[j]: state after j frames of the search (j = 0 is the start state).
    std::vector<uint8_t> seed = DecodeInputs(seedStr);
    std::vector<uint64_t> seedHash;
    std::vector<double> seedScore;
    int64_t seedIdx = -1;
    if (!seed.empty()) {
        if (seed.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), seed.begin())) {
            std::fprintf(stderr, "--seed-route must begin with the --prefix inputs\n");
            return 2;
        }
        auto s = std::make_unique<Sim>(*base);
        for (size_t f = 0; f < prefix.size(); ++f) s->Tick(seed[f]);
        std::vector<uint8_t> buf;
        auto push = [&]() {
            buf.clear();
            EncodeSnapshot(*base, *s, buf);
            seedHash.push_back(Hash64(buf.data(), buf.size()));
            const Body& pb = s->world.bodies[s->playerBody];
            seedScore.push_back(nav.Query(s->spriteX[s->playerBody] + pb.linearVelocity.x * lookahead,
                                          s->spriteY[s->playerBody] + pb.linearVelocity.y * lookahead));
        };
        push();
        for (size_t f = prefix.size(); f < seed.size(); ++f) {
            s->Tick(seed[f]);
            if (s->isTimeStop || !s->playerAlive) break;  // the score pass finds the finish itself
            push();
        }
        if (!resume) seedIdx = 0;  // layer 0 holds exactly the start state
        else
            for (size_t i = 0; i < layer.size() && (size_t)k < seedHash.size(); ++i)
                if (Hash64(layer[i].data(), layer[i].size()) == seedHash[(size_t)k]) seedIdx = (int64_t)i;
        std::printf("seed route: %zu frames kept in the beam\n", seedHash.size() - 1);
    }

    auto save = [&]() {
        const fs::path tmp = fs::path(dir) / "beam.bin.tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            const int lv = level, cp = checkpoint, gl = gless ? 1 : 0;
            const uint32_t plen = (uint32_t)prefixStr.size(), n = (uint32_t)layer.size();
            out.write((const char*)&MAGIC, 4).write((const char*)&lv, 4).write((const char*)&cp, 4).write((const char*)&gl, 4);
            out.write((const char*)&plen, 4).write(prefixStr.data(), plen);
            out.write((const char*)&k, 4).write((const char*)&best, sizeof best).write((const char*)&n, 4);
            for (const auto& e : layer) {
                const uint32_t len = (uint32_t)e.size();
                out.write((const char*)&len, 4).write((const char*)e.data(), len);
            }
        }
        fs::rename(tmp, stateFile);
    };

    // rebuild the full input string of a solution from the link files
    auto routeOf = [&](const Solution& s) {
        std::vector<uint8_t> tail;
        for (int d = 0; d < s.deadFrames; ++d) tail.push_back(IN_NONE);
        tail.push_back(s.input);
        uint32_t idx = s.parent;
        for (int L = s.layer; L > 0; --L) {
            std::ifstream in(fs::path(dir) / "links" / (std::to_string(L) + ".bin"), std::ios::binary);
            in.seekg((std::streamoff)idx * 5);
            uint32_t par = 0;
            uint8_t inp = 0;
            in.read((char*)&par, 4).read((char*)&inp, 1);
            tail.push_back(inp);
            idx = par;
        }
        std::vector<uint8_t> route = prefix;
        route.insert(route.end(), tail.rbegin(), tail.rend());
        return route;
    };

    const auto t0 = std::chrono::steady_clock::now();
    auto lastSave = t0;
    std::mutex solMutex;
    std::vector<std::vector<uint8_t>> encBuf((size_t)threads);
    uint64_t totalChildren = 0;

    for (;; ++k) {
        const int frame = startFrame + k;  // frameCount of the current layer
        if (layer.empty()) {
            std::printf("beam empty at frame %d\n", frame);
            break;
        }
        if (frame + 1 >= best.win || frame + 1 > maxFrames) break;
        // width for the next layer from the memory budget and the current state sizes
        size_t avg = 0;
        for (size_t i = 0; i < layer.size(); i += std::max<size_t>(1, layer.size() / 256)) avg = std::max(avg, layer[i].size());
        size_t W = fixedWidth ? fixedWidth : memory / (2 * (avg + 64) + 8 * sizeof(Cand) + 64);
        W = std::max<size_t>(W, 1);

        // ---- 1. score pass
        std::vector<std::vector<Cand>> local((size_t)threads);
        Parallel(threads, layer.size(), [&](int tid, size_t pi) {
            Sim p;
            DecodeSnapshot(*base, layer[pi].data(), p);
            for (uint8_t c = 0; c < 8; ++c) {
                Sim ch = p;
                ch.Tick(c);
                if (ch.isTimeStop || !ch.playerAlive) {
                    Solution s;
                    int dead = 0;
                    if (!ch.isTimeStop) {  // died: play the death-warp window (inputs are ignored)
                        while (!ch.isTimeStop && ch.frameCount < ch.deathFrame + RESPAWN_LAST_PAUSE_FRAMES) {
                            ch.Tick(IN_NONE);
                            ++dead;
                        }
                    }
                    if (ch.isTimeStop && DeathWarpFinishValid(ch.deathFrame, ch.winFrame)) {
                        s.win = ch.winFrame, s.margin = ch.winMargin, s.death = ch.deathFrame;
                        s.layer = k, s.parent = (uint32_t)pi, s.input = c, s.deadFrames = dead;
                        std::lock_guard<std::mutex> lk(solMutex);
                        if (s.Better(best)) best = s;
                    }
                    continue;
                }
                std::vector<uint8_t>& buf = encBuf[(size_t)tid];
                buf.clear();
                EncodeSnapshot(*base, ch, buf);
                const double bx = ch.spriteX[ch.playerBody], by = ch.spriteY[ch.playerBody];
                const Body& pb = ch.world.bodies[ch.playerBody];
                const int64_t gx = (int64_t)std::floor((bx - nav.x0) / (2 * nav.cell)), gy = (int64_t)std::floor((by - nav.y0) / (2 * nav.cell));
                const int64_t vx = (int64_t)std::lround(pb.linearVelocity.x), vy = (int64_t)std::lround(pb.linearVelocity.y);
                const uint64_t bucket = (uint64_t)((gx * 1000003 + gy) * 1009 + vx) * 1013 + (uint64_t)vy;
                // score: distance to the flag from where the ball would be `lookahead` frames ahead at its
                // current velocity (m/s == px per frame at 30 px/m and 30 steps/s): rewards momentum
                const double score = nav.Query(bx + pb.linearVelocity.x * lookahead, by + pb.linearVelocity.y * lookahead);
                local[(size_t)tid].push_back({score, Hash64(buf.data(), buf.size()), (uint32_t)pi, c, bucket});
            }
        });
        std::vector<Cand> cands;
        for (auto& v : local) cands.insert(cands.end(), v.begin(), v.end());
        totalChildren += cands.size();
        if (frame + 1 >= best.win) break;  // a solution on the next frame: nothing in this layer can beat it

        // ---- 2. select
        // total order on platform-independent keys (the hash covers struct padding, which may differ
        // between compilers, so it is only used to merge duplicates, never to order)
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.score != b.score) return a.score < b.score;
            if (a.parent != b.parent) return a.parent < b.parent;
            return a.input < b.input;
        });
        // Selection. "score": best scores first, at most --diversity per region/velocity bucket.
        // "coverage": round r keeps the r-th best state of every bucket, so an unusual line (behind
        // now, ahead later, or waiting for a crusher) survives while its bucket is distinct.
        // "mixed" (default): half the beam by score, the rest coverage-first.
        std::vector<Cand> keep;
        keep.reserve(std::min(W, cands.size()));
        {
            std::unordered_set<uint64_t> seen;
            seen.reserve(std::min(cands.size(), W * 2));
            std::vector<uint32_t> uniq;  // candidates that are not exact duplicates, in score order
            uniq.reserve(cands.size());
            for (uint32_t i = 0; i < (uint32_t)cands.size(); ++i)
                if (seen.insert(cands[i].hash).second) uniq.push_back(i);
            std::unordered_map<uint64_t, int> rankInBucket;
            std::vector<uint8_t> round(uniq.size());
            for (size_t u = 0; u < uniq.size(); ++u) round[u] = (uint8_t)std::min(255, rankInBucket[cands[uniq[u]].bucket]++);
            std::vector<uint8_t> taken(uniq.size(), 0);
            const size_t scoreQuota = selectMode == 0 ? W : selectMode == 1 ? 0 : W / 2;
            for (size_t u = 0; u < uniq.size() && keep.size() < scoreQuota; ++u)
                if (round[u] < diversity) {
                    keep.push_back(cands[uniq[u]]);
                    taken[u] = 1;
                }
            for (int r = 0; r < diversity && keep.size() < W; ++r)
                for (size_t u = 0; u < uniq.size() && keep.size() < W; ++u)
                    if (round[u] == r && !taken[u]) {
                        keep.push_back(cands[uniq[u]]);
                        taken[u] = 1;
                    }
            // --seed-route: the known route's next state is always kept (its parent is in this layer)
            if (seedIdx >= 0 && (size_t)(startFrame + k) < seed.size()) {
                const uint64_t h = seedHash[(size_t)k + 1];
                bool have = false;
                for (const Cand& c : keep)
                    if (c.hash == h) have = true;
                if (!have && h) {
                    if (keep.size() >= W && !keep.empty()) keep.pop_back();
                    keep.push_back({seedScore[(size_t)k + 1], h, (uint32_t)seedIdx, seed[(size_t)(startFrame + k)], 0});
                }
            }
            std::sort(keep.begin(), keep.end(), [](const Cand& a, const Cand& b) {
                if (a.score != b.score) return a.score < b.score;
                if (a.parent != b.parent) return a.parent < b.parent;
                return a.input < b.input;
            });
            if (seedIdx >= 0) {  // where the seed state sits in the next layer
                const uint64_t h = (size_t)k + 1 < seedHash.size() ? seedHash[(size_t)k + 1] : 0;
                seedIdx = -1;
                for (size_t i = 0; i < keep.size(); ++i)
                    if (h && keep[i].hash == h) seedIdx = (int64_t)i;
            }
        }

        // ---- 3. store pass + links
        std::vector<std::vector<uint8_t>> next(keep.size());
        Parallel(threads, keep.size(), [&](int, size_t i) {
            Sim ch;
            DecodeSnapshot(*base, layer[keep[i].parent].data(), ch);
            ch.Tick(keep[i].input);
            EncodeSnapshot(*base, ch, next[i]);
        });
        {
            std::ofstream out(fs::path(dir) / "links" / (std::to_string(k + 1) + ".bin"), std::ios::binary | std::ios::trunc);
            for (const Cand& c : keep) out.write((const char*)&c.parent, 4).write((const char*)&c.input, 1);
        }
        layer.swap(next);

        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        static double lastPrint = -1e9;
        static int lastBestWin = 1 << 30;
        const bool newSolution = best.win != lastBestWin;
        lastBestWin = best.win;
        if (el - lastPrint < 5 && !newSolution && k > 0) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - lastSave).count() > saveMinutes * 60) {
                ++k;
                save();
                --k;
                lastSave = std::chrono::steady_clock::now();
            }
            continue;
        }
        lastPrint = el;
        size_t bytes = 0;
        for (const auto& e : layer) bytes += e.size() + 32;
        std::printf("frame %5d  width %8zu  best distance %7.1f px  %s  %.0f children/s  states %.0f MB\n", frame + 1, layer.size(),
                    keep.empty() ? 0.0 : keep.front().score,
                    best.win < (1 << 30) ? ("flag on frame " + std::to_string(best.win)).c_str() : "no flag yet",
                    totalChildren / std::max(el, 1e-9), bytes / 1048576.0);
        std::fflush(stdout);
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - lastSave).count() > saveMinutes * 60) {
            ++k;  // the saved layer is k + 1
            save();
            --k;
            lastSave = std::chrono::steady_clock::now();
        }
    }
    save();

    if (best.win >= (1 << 30)) {
        std::printf("no route to the flag found (try a wider beam, --max-frames, or a different start)\n");
        return 1;
    }
    const std::vector<uint8_t> route = routeOf(best);
    // re-check the route from scratch
    auto chk = std::make_unique<Sim>(*base);
    for (uint8_t c : route) {
        chk->Tick(c);
        if (chk->isTimeStop) break;
    }
    const bool ok = chk->isTimeStop && chk->winFrame == best.win && DeathWarpFinishValid(chk->deathFrame, chk->winFrame);
    std::printf("best: flag on frame %d (%.3f s)%s, flag overlap %.0f twips%s\n", best.win, best.win / 31.0,
                best.death >= 0 ? " via death warp" : "", best.margin, ok ? "" : "  ** REPLAY MISMATCH **");
    if (best.death >= 0)
        std::printf("death warp: death on frame %d, flag on frame %d; pause by frame %d, unpause on frame %d\n", best.death, best.win,
                    best.death + RESPAWN_LAST_PAUSE_FRAMES, best.win + WIN_TIMER_FRAMES);
    if (chk->displayUncertain) std::printf("note: a hit test on this route was within one matrix unit of flipping: check it in Flash\n");
    std::printf("%s\n", EncodeInputsRLE(route).c_str());
    return ok ? 0 : 1;
}
