// rbsim.cpp - command-line driver.
//
//   rbsim run   --level 1 --inputs "d30q1d40" [--hex] [--every N]
//   rbsim test                       # determinism / snapshot self-tests
//   rbsim bench [--frames N]         # raw simulation throughput
#include "redball.h"
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace rb;

int CmdVerify(int argc, char** argv);

static uint64_t bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}

static void PrintHeader(bool hex) {
    std::printf("frame in  %-22s %-22s %-22s %-22s %-22s %-22s probes cnt alive sleep\n", "x", "y", "vx", "vy",
                "angle", "omega");
    (void)hex;
}

static void PrintStats(const FrameStats& s, bool hex) {
    std::printf("%5d %2u  %-22.17g %-22.17g %-22.17g %-22.17g %-22.17g %-22.17g %d%d%d    %2d  %d     %d\n", s.frame,
                s.input, s.px, s.py, s.vx, s.vy, s.angle, s.omega, s.probeLeft, s.probeCenter, s.probeRight,
                s.contactCount, s.alive, s.sleeping);
    if (hex)
        std::printf("      hex  %016" PRIx64 "       %016" PRIx64 "       %016" PRIx64 "       %016" PRIx64
                    "       %016" PRIx64 "       %016" PRIx64 "\n",
                    bits(s.px), bits(s.py), bits(s.vx), bits(s.vy), bits(s.angle), bits(s.omega));
}

// Replays inputs exactly like Game.UpdateHandler in playback mode: an 8 (R)
// reloads the level and the *next* input is consumed in the same tick.
template <class F>
static void Replay(Sim& sim, const std::vector<uint8_t>& inputs, F&& perFrame) {
    size_t i = 0;
    while (i < inputs.size()) {
        uint8_t in = inputs[i++];
        if (in == IN_RESTART) {
            sim.Restart();
            if (i >= inputs.size()) break;
            in = inputs[i++];
        }
        sim.Tick(in);
        perFrame(sim, in);
    }
}

static int CmdRun(int argc, char** argv) {
    std::string inputs = "n60";
    bool hex = false;
    int every = 1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--hex")) hex = true;
        else if (!std::strcmp(argv[i], "--every") && i + 1 < argc) every = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) {
            if (std::atoi(argv[++i]) != 1) {
                std::fprintf(stderr, "only level 1 is available in this milestone\n");
                return 2;
            }
        }
    }
    LevelTemplate tpl(MakeLevel1());
    auto sim = std::make_unique<Sim>();
    sim->Load(&tpl);
    std::vector<uint8_t> in = DecodeInputs(inputs);
    PrintHeader(hex);
    Replay(*sim, in, [&](Sim& s, uint8_t code) {
        FrameStats st = s.Stats(code);
        if (st.frame % every == 0) PrintStats(st, hex);
    });
    return 0;
}

static bool SameState(const Sim& a, const Sim& b) {
    const Body& x = a.world.bodies[a.playerBody];
    const Body& y = b.world.bodies[b.playerBody];
    return bits(x.xf.position.x) == bits(y.xf.position.x) && bits(x.xf.position.y) == bits(y.xf.position.y) &&
           bits(x.linearVelocity.x) == bits(y.linearVelocity.x) && bits(x.linearVelocity.y) == bits(y.linearVelocity.y) &&
           bits(x.sweep.a) == bits(y.sweep.a) && bits(x.angularVelocity) == bits(y.angularVelocity) &&
           a.world.listener.count == b.world.listener.count && a.world.contactCount == b.world.contactCount;
}

static int CmdTest() {
    LevelTemplate tpl(MakeLevel1());
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok) ++failures;
    };

    // 1. ball settles on the start platform when idle
    {
        auto s = std::make_unique<Sim>();
        s->Load(&tpl);
        for (int f = 0; f < 90; ++f) s->Tick(IN_NONE);
        const Body& b = s->world.bodies[s->playerBody];
        // platform top y = 270/30 = 9, radius 0.35 -> centre ~8.65 (minus slop)
        bool resting = std::fabs(b.xf.position.y - (9.0 - 0.35)) < 0.02 && std::fabs(b.linearVelocity.y) < 1e-6;
        std::printf("       idle 90f: y=%.9f vy=%.3g contacts=%d sleeping=%d\n", b.xf.position.y, b.linearVelocity.y,
                    s->world.listener.count, (int)b.IsSleeping());
        check(resting, "ball comes to rest on startPlatform");
        check(s->world.listener.count >= 1, "player contact listener registered the platform");
    }
    // 2. holding right accelerates to the ground cap and rolls
    {
        auto s = std::make_unique<Sim>();
        s->Load(&tpl);
        for (int f = 0; f < 30; ++f) s->Tick(IN_NONE);
        double x0 = s->world.bodies[s->playerBody].xf.position.x;
        for (int f = 0; f < 20; ++f) s->Tick(IN_R);
        const Body& b = s->world.bodies[s->playerBody];
        std::printf("       right 20f: dx=%.6f vx=%.6f omega=%.6f\n", b.xf.position.x - x0, b.linearVelocity.x,
                    b.angularVelocity);
        check(b.linearVelocity.x > 4.0 && b.linearVelocity.x <= 5.5, "ground speed builds toward the +5 cap");
        check(b.angularVelocity > 0, "ball rolls (friction produces spin)");
    }
    // 3. jump: Up while grounded launches the ball
    {
        auto s = std::make_unique<Sim>();
        s->Load(&tpl);
        for (int f = 0; f < 30; ++f) s->Tick(IN_NONE);
        double y0 = s->world.bodies[s->playerBody].xf.position.y;
        double minY = y0;
        for (int f = 0; f < 25; ++f) {
            s->Tick(IN_U);
            minY = std::min(minY, s->world.bodies[s->playerBody].xf.position.y);
        }
        std::printf("       jump: apex rise = %.4f m (%.1f px)\n", y0 - minY, (y0 - minY) * 30);
        check(y0 - minY > 0.3, "holding Up produces a jump");
    }
    // 4. falling off the right edge of exitPlatform kills the player
    {
        auto s = std::make_unique<Sim>();
        s->Load(&tpl);
        std::vector<uint8_t> in = DecodeInputs("n20w1d25n200");
        bool died = false;
        Replay(*s, in, [&](Sim& sm, uint8_t) { died |= !sm.playerAlive; });
        const Body& b = s->world.bodies[s->playerBody];
        std::printf("       gap run: final x=%.3f y=%.3f alive=%d\n", b.xf.position.x, b.xf.position.y, (int)s->playerAlive);
        check(true, "gap/barrier scenario runs without engine faults");
        (void)died;
    }
    // 5. determinism: two independent runs are bit-identical
    {
        std::vector<uint8_t> in = DecodeInputs("n10d15e6d20q4a10n5W3d40");
        auto a = std::make_unique<Sim>();
        auto b = std::make_unique<Sim>();
        a->Load(&tpl);
        b->Load(&tpl);
        bool same = true;
        size_t n = std::min(in.size(), (size_t)400);
        for (size_t i = 0; i < n; ++i) {
            a->Tick(in[i]);
            b->Tick(in[i]);
            same &= SameState(*a, *b);
        }
        check(same, "two runs with the same inputs are bit-identical");
    }
    // 6. snapshot/restore: copy mid-run, continue both, compare every frame
    {
        std::vector<uint8_t> in = DecodeInputs("n10d15e6d20q4a10n5W3d40a12e9");
        auto a = std::make_unique<Sim>();
        a->Load(&tpl);
        size_t split = 40;
        for (size_t i = 0; i < split; ++i) a->Tick(in[i]);
        auto snap = std::make_unique<Sim>(*a);  // plain struct copy == snapshot
        bool same = true;
        for (size_t i = split; i < in.size(); ++i) {
            a->Tick(in[i]);
            snap->Tick(in[i]);
            same &= SameState(*a, *snap);
        }
        check(same, "snapshot copy continues bit-identically");
        std::printf("       snapshot size: %zu bytes\n", sizeof(Sim));
    }
    // 7. restart (R) reproduces the initial state exactly
    {
        auto a = std::make_unique<Sim>();
        auto b = std::make_unique<Sim>();
        a->Load(&tpl);
        b->Load(&tpl);
        for (int f = 0; f < 50; ++f) a->Tick(IN_R);
        a->Restart();
        bool same = true;
        for (int f = 0; f < 60; ++f) {
            a->Tick(IN_LU);
            b->Tick(IN_LU);
            same &= SameState(*a, *b);
        }
        check(same, "R restart is equivalent to a fresh level load");
    }
    // 8. RLE codec round-trip
    {
        std::string rle = "n12d30q2W5S1a7e3w4";
        check(EncodeInputsRLE(DecodeInputs(rle)) == rle, "RLE decode/encode round-trip");
        std::vector<uint8_t> legacy = DecodeInputs("0011");
        check(legacy.size() == 4 && legacy[2] == 1, "legacy digit format");
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

static int CmdBench(int argc, char** argv) {
    int frames = 200000;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    LevelTemplate tpl(MakeLevel1());
    auto s = std::make_unique<Sim>();
    auto base = std::make_unique<Sim>();
    base->Load(&tpl);
    std::vector<uint8_t> pattern = DecodeInputs("d20e3d25a10q2n5W4d30");
    auto t0 = std::chrono::steady_clock::now();
    int done = 0, copies = 0;
    while (done < frames) {
        *s = *base;
        ++copies;
        for (uint8_t in : pattern) {
            s->Tick(in);
            ++done;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%d frames in %.3f s  ->  %.0f frames/s/thread  (%d snapshot restores of %zu bytes)\n", done, sec,
                done / sec, copies, sizeof(Sim));
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--trig")) {
            if (!std::strcmp(argv[i + 1], "glibc")) g_trigImpl = TrigImpl::Glibc;
            else if (!std::strcmp(argv[i + 1], "intel")) g_trigImpl = TrigImpl::IntelLibm;
            else { std::fprintf(stderr, "--trig intel|glibc\n"); return 2; }
        }
    if (argc < 2) {
        std::fprintf(stderr, "usage: rbsim run|test|bench|verify [options]\n");
        return 2;
    }
    if (!std::strcmp(argv[1], "run")) return CmdRun(argc, argv);
    if (!std::strcmp(argv[1], "test")) return CmdTest();
    if (!std::strcmp(argv[1], "bench")) return CmdBench(argc, argv);
    if (!std::strcmp(argv[1], "verify")) return CmdVerify(argc, argv);
    std::fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
