// Source-only regression runner; compile separately when a build is authorized.
// Link the engine sources, but NOT smartforce.cpp or rbsim.cpp separately.
#include "../src/smartforce.cpp"
#include <iostream>

static void Require(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}

static void SameState(const State& a, const State& b) {
    Require(std::memcmp(&a.sim, &b.sim, sizeof(Sim)) == 0, "complete Sim differs");
    Require(std::memcmp(a.met, b.met, sizeof a.met) == 0, "report progress differs");
    Require(a.played == b.played, "played route differs");
}

static void Configure(Force& f, const std::string& pattern, const std::vector<std::string>& groups) {
    double bytes = 0;
    std::string error;
    std::vector<JumpSummary> jumps;
    f.segs = ParsePattern(pattern, 1e8, bytes, error, jumps);
    Require(error.empty(), "pattern rejected");
    f.rest.assign(f.segs.size() + 1, 1);
    for (size_t i = f.segs.size(); i-- > 0;) f.rest[i] = f.rest[i+1] * (double)f.segs[i].size();
    for (const std::string& text : groups) {
        Cond c;
        Require(ParseCond(text, c, error), "condition rejected");
        const size_t index = f.conds.size();
        f.conds.push_back(c);
        f.groups.push_back({index, index+1, c.hi, CanHoldDead(c)});
    }
    for (size_t i = f.groups.size(); i-- > 1;) f.groups[i-1].deadLater &= f.groups[i].deadLater;
}

static std::unique_ptr<State> Start(LevelTemplate& tpl) {
    auto state = std::make_unique<State>();
    state->sim.Load(&tpl);
    std::memset(state->met, 0, sizeof state->met);
    return state;
}

int main() try {
    g_stop = false;
    LevelTemplate tpl(17);
    auto base = Start(tpl);
    const std::string route = "d4n1a1e1d10e2d5n2d1a2n1d1n3d10w1d2e1d35e3d21e19d19e1d14e1d54e1w9n1d9e3d2e1d1e33d1e1d2e8d9";
    for (int mode = 0; mode < 3; ++mode) {
        Force cached, original;
        Configure(cached, route + "d0..10", {"alive@1", "alive@298"});
        Configure(original, route + "d0..10", {"alive@1", "alive@298"});
        auto cache = std::make_unique<DeterministicPrefixes>(8 * sizeof(State), 4);
        cached.deterministicPrefixes = cache.get();
        auto seen1 = std::make_unique<Seen>(), seen2 = std::make_unique<Seen>();
        auto canonical = std::make_unique<Sim>();
        if (mode) {
            Canonicalize(base->sim, *canonical, true);
            cached.seen = seen1.get(); original.seen = seen2.get();
            seen1->cap = seen2->cap = 10000;
            cached.rawStates = original.rawStates = mode == 2;
            cached.keyBase = original.keyBase = mode == 2 ? &base->sim : canonical.get();
            cached.tol = original.tol = 0;
            cached.mergeEvery = original.mergeEvery = 7;
        }
        Walker a(cached), b(original);
        for (uint64_t tail : {11ULL, 22ULL, 11ULL}) {
            auto x = Start(tpl), y = Start(tpl);
            const bool ax = a.PlayPrefix(*x, {0}, 0, 1, 1, tail);
            const bool by = b.PlayPrefix(*y, {0}, 0, 1, 1, tail);
            Require(ax == by && a.ended == b.ended, "prefix outcome differs");
            if (!ax) { Require(a.ended == MERGED, "unexpected prefix failure"); continue; }
            SameState(*x, *y);
            Require(a.Step(*x, IN_UR) == b.Step(*y, IN_UR), "jump outcome differs");
            SameState(*x, *y);
            Require(a.Step(*x, IN_NONE) == b.Step(*y, IN_NONE), "jump continuation differs");
            SameState(*x, *y);
        }
        Require(cache->built == 1 && cache->reused == 2, "expected shared endpoint not reused");
    }
    for (const std::string& condition : {"alive@20", "px>1e30@20"}) {
        Force cached, original;
        Configure(cached, route + "d0..10", {condition});
        Configure(original, route + "d0..10", {condition});
        auto cache = std::make_unique<DeterministicPrefixes>(8 * sizeof(State), 4);
        cached.deterministicPrefixes = cache.get();
        Walker a(cached), b(original);
        for (double weight : {7.0, 3.0}) {
            auto x = Start(tpl), y = Start(tpl);
            Require(a.PlayPrefix(*x, {0}, 0, 1, weight) == b.PlayPrefix(*y, {0}, 0, 1, weight), "early outcome differs");
            Require(a.ended == b.ended, "early status differs");
            SameState(*x, *y);
        }
        Require(a.rows.matched == b.rows.matched, "early match multiplicity differs");
    }
    for (int choices : {9, 10}) {
        Force f;
        Configure(f, "n40d0.." + std::to_string(choices-1), {"alive@50"});
        auto cache = std::make_unique<DeterministicPrefixes>(8 * sizeof(State), 4);
        f.deterministicPrefixes = cache.get();
        Walker w(f);
        for (int i = 0; i < 2; ++i) { auto s = Start(tpl); w.PlayPrefix(*s, {0}, 0, 1, 1); }
        Require(cache->built == (choices == 10 ? 1U : 0U), "10x gate differs");
        const uint64_t before = cache->reused;
        std::atomic<uint64_t> played{0};
        f.played = &played;
        f.sampling = true;
        auto s = Start(tpl);
        w.PlayPrefix(*s, {0}, 0, 1, 1);
        Require(cache->reused == before, "stochastic used deterministic cache");
    }
    {
        Force cached, original;
        Configure(cached, "(d2|a2)n40d0..10", {"alive@60"});
        Configure(original, "(d2|a2)n40d0..10", {"alive@60"});
        auto cache = std::make_unique<DeterministicPrefixes>(8 * sizeof(State), 4);
        cached.deterministicPrefixes = cache.get();
        Walker a(cached), b(original);
        for (uint64_t choice : {0ULL, 1ULL, 0ULL, 1ULL}) {
            auto x = Start(tpl), y = Start(tpl);
            Require(a.PlayPrefix(*x, {choice, 0}, 0, 2, 1) == b.PlayPrefix(*y, {choice, 0}, 0, 2, 1), "history outcome differs");
            SameState(*x, *y);
        }
        Require(cache->built == 4 && cache->reused == 4, "prefix histories were aliased");
    }
    {
        const size_t cost = DeterministicPrefixes::Cost(0, 0);
        auto cache = std::make_unique<DeterministicPrefixes>(sizeof(DeterministicPrefixes) + cost, 12);
        std::atomic<int> filled{0};
        auto fill = [&](DeterministicPrefixes::Entry& e) { ++filled; CopyState(e.state, *base); };
        std::vector<std::thread> threads;
        std::atomic<bool> identical{true};
        for (int i = 0; i < 12; ++i) threads.emplace_back([&] {
            auto entry = cache->Get({0, 0}, cost, fill);
            if (!entry || std::memcmp(&entry->state.sim, &base->sim, sizeof(Sim))) identical = false;
        });
        for (auto& thread : threads) thread.join();
        Require(identical && filled == 1, "12 workers duplicated or corrupted a snapshot");
        auto pinned = cache->Get({0, 0}, cost, fill);
        Require(!cache->Get({0, 1}, cost, fill), "pinned snapshot exceeded budget");
        pinned.reset();
        Require(bool(cache->Get({0, 1}, cost, fill)), "idle entry was not evicted");
        Require(cache->evicted == 1 && cache->used <= cache->capacity, "cache budget differs");
    }
    for (size_t limit : {size_t(1), size_t(12)}) {
        const size_t cost = DeterministicPrefixes::Cost(0, 0);
        // Enough bytes for extra entries: only the thread-count cap can stop them.
        auto cache = std::make_unique<DeterministicPrefixes>(sizeof(DeterministicPrefixes) + (limit + 2) * cost, limit);
        auto fill = [&](DeterministicPrefixes::Entry& e) { CopyState(e.state, *base); };
        std::vector<std::shared_ptr<const DeterministicPrefixes::Entry>> pinned;
        for (size_t i = 0; i < limit; ++i) {
            pinned.push_back(cache->Get({0, i}, cost, fill));
            Require(bool(pinned.back()), "thread-cap slot missing");
        }
        Require(!cache->Get({0, limit}, cost, fill), "pinned entries exceeded thread cap");
        pinned[0].reset();
        auto replacement = cache->Get({0, limit}, cost, fill);
        Require(bool(replacement), "unused entry not replaced at thread cap");
        Require(cache->entries.size() == limit && cache->peakEntries == limit && cache->evicted == 1,
                "thread cap or eviction count differs");
    }
    std::cout << "PASS exact prefix states, continuation, reports, hardfilter, 10x gate, history keys, concurrency, eviction and thread cap\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
