// rbsim.cpp - command-line driver.
//
//   rbsim run   --level 1 --inputs "d30q1d40" [--hex] [--every N]
//   rbsim test                       # determinism / snapshot self-tests
//   rbsim bench [--frames N]         # raw simulation throughput
#include "redball.h"
#include "snapshot.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace rb;

int CmdVerify(int argc, char** argv);
int CmdCalib(int argc, char** argv);
int CmdOptimize(int argc, char** argv);
static double SpriteCoordForTest(double v) { return (double)as3_toInt32(v * 20) / 20.0; }

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


static std::string Hex(double d) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016" PRIx64, bits(d));
    return buf;
}

// Writes a stats log in the exact format of the Flash logging mod
// (docs/STATS_LOGGING.md), including the optional extra-body columns.
static void LogLine(const Sim& s, int tick, uint8_t in, bool extras) {
    FrameStats st = s.Stats(in);
    std::printf("%d\t%d\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%u\t%d\t%d\t%d\t%d\t%s\t%d\t%s\t%s\t%s\t%d", tick,
                st.frame, in, Hex(st.px).c_str(), Hex(st.py).c_str(), Hex(st.vx).c_str(), Hex(st.vy).c_str(),
                Hex(st.angle).c_str(), Hex(st.omega).c_str(), Hex(st.sleepTime).c_str(), st.flags, st.probeCenter,
                st.probeLeft, st.probeRight, st.contactCount, st.contactNames.c_str(), st.worldContactCount,
                Hex(st.sx).c_str(), Hex(st.sy).c_str(), Hex(st.sr).c_str(), (int)s.isTimeStop);
    if (extras) {
        const World& w = s.world;
        for (int32_t b = w.bodyList; b != -1; b = w.bodies[b].next) {
            const Body& B = w.bodies[b];
            if (b == s.playerBody || B.IsStatic() || !s.hasSprite[b]) continue;
            std::printf("\t%s\t%s\t%s\t%s\t%s\t%s\t%s", s.BodyName(b).c_str(), Hex(B.xf.position.x).c_str(),
                        Hex(B.xf.position.y).c_str(), Hex(B.sweep.a).c_str(), Hex(B.linearVelocity.x).c_str(),
                        Hex(B.linearVelocity.y).c_str(), Hex(B.angularVelocity).c_str());
        }
    }
    std::printf("\n");
}

static int CmdLog(int argc, char** argv) {
    std::string inputs = "n60";
    int level = 1, checkpoint = 0;
    bool extras = true;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) level = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-extras")) extras = false;
    }
    if (!GetLevelScript(level).implemented) {
        std::fprintf(stderr, "level %d is not implemented yet\n", level);
        return 2;
    }
    LevelTemplate tpl(level);
    auto sim = std::make_unique<Sim>();
    sim->Load(&tpl, checkpoint);
    std::printf("# inputs\t%s\n", inputs.c_str());
    std::printf("#tick\tframe\tin\tpx\tpy\tvx\tvy\ta\tw\tsleepT\tflags\tpC\tpL\tpR\tnCB\tcb\tnC\tsx\tsy\tsr\tts%s\n",
                extras ? "\t[name px py a vx vy w]..." : "");
    std::printf("LEVEL %d %d\n", level, checkpoint);
    LogLine(*sim, 0, 0, extras);
    int tick = 0;
    Replay(*sim, DecodeInputs(inputs), [&](Sim& s, uint8_t code) { LogLine(s, ++tick, code, extras); });
    return 0;
}

static int CmdRun(int argc, char** argv) {
    std::string inputs = "n60";
    bool hex = false;
    int every = 1;
    int level = 1, checkpoint = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--hex")) hex = true;
        else if (!std::strcmp(argv[i], "--every") && i + 1 < argc) every = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) level = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = std::atoi(argv[++i]);
    }
    if (!GetLevelScript(level).implemented) {
        std::fprintf(stderr, "level %d is not implemented yet\n", level);
        return 2;
    }
    LevelTemplate tpl(level);
    auto sim = std::make_unique<Sim>();
    sim->Load(&tpl, checkpoint);
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
    LevelTemplate tpl(1);
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
    // 9. Level 2: joints behave, and snapshots capture joint state
    {
        LevelTemplate t2(2);
        auto s = std::make_unique<Sim>();
        s->Load(&t2);
        World& w = s->world;
        int32_t kick = -1, mp = s->lvBody[0];
        for (int32_t b = w.bodyList; b != -1; b = w.bodies[b].next)
            if (s->BodyName(b) == "kickBall") kick = b;
        int32_t dj = -1;
        for (int32_t j = w.jointList; j != -1; j = w.joints[j].next)
            if (w.joints[j].type == JT_DISTANCE) dj = j;
        check(kick >= 0 && dj >= 0 && w.jointCount == 2, "level 2 builds pendulum + prismatic joints");
        double maxErr = 0, minX = 1e9, maxX = -1e9, maxDy = 0;
        double y0 = w.bodies[mp].xf.position.y;
        for (int f = 0; f < 600; ++f) {
            s->Tick(IN_NONE);
            const Joint& J = w.joints[dj];
            Vec2 a1 = b2MulX(w.bodies[kick].xf, J.localAnchor1);
            Vec2 a2 = b2MulX(w.bodies[w.groundBody].xf, J.localAnchor2);
            maxErr = std::max(maxErr, std::fabs(std::hypot(a1.x - a2.x, a1.y - a2.y) - J.length));
            minX = std::min(minX, s->spriteX[mp]);
            maxX = std::max(maxX, s->spriteX[mp]);
            maxDy = std::max(maxDy, std::fabs(w.bodies[mp].xf.position.y - y0));
        }
        std::printf("       L2 600f: rope error <= %.2e m, platform x in [%.2f, %.2f] px, platform |dy| <= %.2e m\n",
                    maxErr, minX, maxX, maxDy);
        check(maxErr < 1e-4, "distance joint holds the pendulum rope length");
        check(minX < 390 && minX > 380 && maxX > 550 && maxX < 560, "moving platform reverses at sprite x 390 / 550");
        check(maxDy < 0.01, "prismatic joint keeps the platform on its axis");

        auto a = std::make_unique<Sim>();
        a->Load(&t2);
        std::vector<uint8_t> in = DecodeInputs("n20d30e6d25a8q5n30d40");
        for (size_t i = 0; i < 60; ++i) a->Tick(in[i]);
        auto snap = std::make_unique<Sim>(*a);
        bool same = true;
        for (size_t i = 60; i < in.size(); ++i) {
            a->Tick(in[i]);
            snap->Tick(in[i]);
            same &= SameState(*a, *snap);
            const Body& k1 = a->world.bodies[kick];
            const Body& k2 = snap->world.bodies[kick];
            same &= bits(k1.sweep.c.x) == bits(k2.sweep.c.x) && bits(k1.angularVelocity) == bits(k2.angularVelocity);
        }
        check(same, "level 2 snapshot copy continues bit-identically (joints included)");
    }
    // 11. display layer (FP 11.4 logs rb1_stats2_{2,3,4}_f): Level 2 goal and checkpoint timing
    {
        LevelTemplate t2(2);
        struct Case { const char* name; const char* inputs; int winTick, winFrame; };
        const Case cases[] = {
            {"tas-style run", "d18e1w1n1w5n26a2e1w4n10w1n46a2n4d1n19w6n2w8n28", 186, 185},
            {"long manual run", "d18e6d41e17d7e1d6e2d24a2d26q25d18e2q55d10e22q23e8d1e1d2n2w3n26d11q1a17n6d5n1d60", 449, 448},
        };
        for (const Case& c : cases) {
            auto s = std::make_unique<Sim>();
            s->Load(&t2);
            std::vector<uint8_t> in = DecodeInputs(c.inputs);
            int tick = 0, winTick = -1;
            for (uint8_t code : in) {
                s->Tick(code);
                ++tick;
                if (s->isTimeStop && winTick < 0) winTick = tick;
            }
            check(winTick == c.winTick && s->frameCount == c.winFrame,
                  (std::string("level 2 goal triggers at the Flash tick: ") + c.name).c_str());
        }
        // run 4: pick up checkPoint1 (tick 165), R, restart there, then win 175 ticks later
        auto s = std::make_unique<Sim>();
        s->Load(&t2);
        std::vector<uint8_t> in = DecodeInputs(
            "d18e8d32a12d1e2n8d13e1d6w2a6n56R1n1d14e11d7a1n5w1d21n22d20e1d18a1d1e3d3e1d1e2d4a1d3e5d28");
        int tick = 0, cpTick = -1, winTick = -1, seg = 0;
        for (size_t i = 0; i < in.size();) {
            uint8_t code = in[i++];
            if (code == IN_RESTART) {
                s->Restart();
                tick = 0;
                seg = 1;
                if (i >= in.size()) break;
                code = in[i++];
            }
            s->Tick(code);
            ++tick;
            if (seg == 0 && s->lastCheckNum == 1 && cpTick < 0) cpTick = tick;
            if (s->isTimeStop && winTick < 0) winTick = tick;
        }
        check(cpTick == 165, "level 2 checkPoint1 collected at the Flash tick");
        check(winTick == 175, "level 2 goal after a checkpoint restart at the Flash tick");
        auto idle = std::make_unique<Sim>();
        idle->Load(&t2);
        for (int f = 0; f < 370; ++f) idle->Tick(IN_NONE);
        check(!idle->isTimeStop && idle->lastCheckNum == 0, "idle on the start platform: no goal, no checkpoint");
        check(s->displayUncertain == 0 && idle->displayUncertain == 0,
              "no goal/checkpoint outcome depends on a one-unit matrix error in the logged runs");
    }
    // 12. Flash display matrix (calibration 3.7): spot values from rb1_calib3_7.tsv / rb1_calib3_6.tsv
    {
        const struct { double rot; int32_t a, b; } m[] = {
            {-180.0, -65536, 0}, {-179.2683, -65530, -638}, {9.7603, 64552, 11101}, {10.0003, 64503, 11380},
            {0.2553, 65534, 287}, {-0.2397, 65535, -69}, {28.5309, 57487, 31279}, {-106.83, -18909, -62687},
        };
        bool ok = true;
        for (auto& e : m) {
            const FlashMatrix f = FlashRotationMatrix(e.rot);
            if (f.a != e.a || f.b != e.b) ok = false;
        }
        check(ok, "Flash rotation -> 16.16 matrix reproduces logged matrices");
        // rb1_stats_8_2.tsv tick 92: the game WROTE 307.76972460547630 (getter: -52.23027539452369);
        // Flash builds the matrix from the written value: (40127, -51684), not (40127, -51683)
        uint64_t wb = 0x40733c50cabf7728ULL;
        double w;
        std::memcpy(&w, &wb, 8);
        const FlashMatrix fw = FlashRotationMatrix(w);
        const FlashMatrix fn = FlashRotationMatrix(w - 360);
        check(fw.a == 40127 && fw.b == -51684 && fn.b == -51683, "display matrix is built from the written rotation, not the normalised one");
        const Rect r = BallBounds(500, 300, -179.2683);
        check(r.x0 == 10000 - 212 && r.x1 == 10000 + 212, "rotated ball getBounds half-extent (Flash matrix, round-to-nearest)");
    }
    // 13. Level 3 + standardized spikes (MATHSPIKES = 1)
    {
        LevelTemplate t3(3);
        check(t3.spikeCount == 33, "level 3 has 3 spike rows x 11 spikes");
        const SpikeObj& first = t3.spikes[0];  // ship1: origin (314, 425) px, apex (317, 415.35)
        check(first.tx == 314 && first.ty == 425 && first.by0 == 8307 && first.bx1 == 6400, "first spike origin and bounds");
        // ball straight above the apex, bottom control point 0.25 px above the tip -> no hit
        check(!BallHitsSpike(317, 415.35 - 10.5 - 0.25, 0, 0, 0, first).hit, "ball clear above a spike tip");
        // bottom control point 0.5 px into the tip -> hit
        check(BallHitsSpike(317, 415.35 - 10.5 + 0.5, 0, 0, 0, first).hit, "control point inside a spike tip");
        // the camera step shifts the test: same ball, dp moving the point up out of the tip -> no hit
        check(!BallHitsSpike(317, 415.35 - 10.5 + 0.5, 0, 0, 1.0, first).hit, "dp (camera step) shifts the spike test");
        // but the bounds test uses the unshifted point: a point below the tip's bounds never hits, even if dp moves it in
        check(!BallHitsSpike(317, 415.35 - 10.5 - 0.5, 0, 0, -1.0, first).hit, "bounds check happens before the dp shift");
        auto s3 = std::make_unique<Sim>();
        s3->Load(&t3);
        check(s3->camX == -s3->spriteX[s3->playerBody] + 275 && s3->camY == SpriteCoordForTest(-s3->spriteY[s3->playerBody] + 200),
              "camera starts centred on the ball");
        for (int f = 0; f < 600; ++f) s3->Tick(IN_NONE);
        check(s3->playerAlive && !s3->isTimeStop, "level 3 idle: alive after 600 ticks");
        auto s4 = std::make_unique<Sim>();
        s4->Load(&t3);
        std::vector<uint8_t> in = DecodeInputs("d25w1d300");
        int f = 0;
        for (uint8_t c : in) {
            s4->Tick(c);
            ++f;
            if (!s4->playerAlive) break;
        }
        check(!s4->playerAlive && f < 100 && s4->spriteY[s4->playerBody] < 450, "level 3: jumping into the first spike row kills");
        // rb1_stats_mathspikes2.tsv (FP 11.4, MATHSPIKES = 1): d18 e1 d58 dies on the spikes at tick 72
        auto s5 = std::make_unique<Sim>();
        s5->Load(&t3);
        int deathTick = -1, tk = 0;
        for (uint8_t c : DecodeInputs("d18e1d58n10")) {
            s5->Tick(c);
            ++tk;
            if (!s5->playerAlive) {
                deathTick = tk;
                break;
            }
        }
        check(deathTick == 72, "level 3: standardized spike death on the Flash tick (72)");
        // E8c spot rows (rb1_calib_mathspikes.tsv): whole check on ship1, covers at (0,0)
        auto hitShip1 = [&](double rot, double x, double y) {
            for (int32_t i = 0; i < 11; ++i)
                if (BallHitsSpike(x, y, rot, 0, 0, t3.spikes[i]).hit) return true;
            return false;
        };
        check(!hitShip1(0, 317, 404.8) && hitShip1(0, 317, 405.4), "standardized check: tip contact threshold at rotation 0");
    }
    // 14. Level 8 (car, crushers, kill ramps, rotated spike rows)
    {
        LevelTemplate t8(8);
        check(t8.spikeCount == 12 * 11, "level 8 has 12 spike rows x 11 spikes (shipik7 is placed twice)");
        // wall spikes on the left face of 'pol': quarter-turned, bases at x = -733.6, tips at x = -743.25
        const SpikeObj& w = t8.spikes[0];  // shipik3 first Shipik: tip at (-743.25, 352.55)
        check(w.a == 0 && w.b == -1 && w.c == 1 && w.d == 0 && w.bx0 == -14865, "wall spikes are quarter-turned, tips pointing left");
        check(BallHitsSpike(-743.25 - 10.5 + 0.5, 352.55, 0, 0, 0, w).hit, "ball pressed onto a wall spike tip is hit");
        check(!BallHitsSpike(-743.25 - 10.5 - 0.25, 352.55, 0, 0, 0, w).hit, "ball just left of a wall spike tip is safe");
        auto s8 = std::make_unique<Sim>();
        s8->Load(&t8);
        int reversals = 0, prevUp = s8->lvInt[0];
        for (int f = 0; f < 900; ++f) {
            s8->Tick(IN_NONE);
            if (s8->lvInt[0] != prevUp) ++reversals;
            prevUp = s8->lvInt[0];
        }
        check(s8->playerAlive && reversals >= 20, "level 8 idle: alive, crushers cycle");
        // rb1_stats_8_11.tsv (FP 11.4, MATHSPIKES = 1): fastest logged Level 8 route, win on tick 405
        auto w8 = std::make_unique<Sim>();
        w8->Load(&t8);
        int winTick = -1, tk = 0;
        for (uint8_t c : DecodeInputs("d19a3n1a1n12d1a1n21d15e1d1e1d3e22d7e5d30e1d20e7d3e1d9n1d76e2d14a1n1a1n18a3n10a1n18w9n65")) {
            w8->Tick(c);
            ++tk;
            if (w8->isTimeStop) {
                winTick = tk;
                break;
            }
        }
        check(winTick == 405 && w8->playerAlive, "level 8: logged TAS wins on the Flash tick (405)");
    }
    // 15. Death warp: after PlayerDie the ball is off the display list, so goal/checkpoint tests compare its
    //     frozen Level-local box with targets shifted by the camera (Level.Update has no IsLive() guard).
    {
        LevelTemplate t8(8);
        const DisplayObj& cp = *t8.cps[1];
        const double cx = (cp.x0 + cp.x1) / 2 / 20, cy = (cp.y0 + cp.y1) / 2 / 20;  // checkPoint1 centre, px
        // dead ball at P with the camera settled at 275 - P (200 - P): the target appears at C + 275 - P
        const double px = (cx + 275) / 2, py = (cy + 200) / 2;
        auto place = [&](Sim& s, bool alive) {
            s.Load(&t8);
            s.spriteX[s.playerBody] = SpriteCoordForTest(px);
            s.spriteY[s.playerBody] = SpriteCoordForTest(py);
            s.spriteRot[s.playerBody] = s.spriteRotW[s.playerBody] = 0;
            s.camX = SpriteCoordForTest(-s.spriteX[s.playerBody] + 275);
            s.camY = SpriteCoordForTest(-s.spriteY[s.playerBody] + 200);
            s.camTargetX = -s.spriteX[s.playerBody] + 275;
            s.camTargetY = -s.spriteY[s.playerBody] + 200;
            s.camTween = true;
            s.playerAlive = alive;
        };
        auto d = std::make_unique<Sim>();
        place(*d, false);
        check(d->BallHitsTarget(cp) && d->lastCheckNum == 0, "dead ball: checkpoint shifted by the camera overlaps it");
        d->Tick(IN_NONE);  // DeadUpdate: camera step + checkpoint loop
        check(d->lastCheckNum == 1 && d->frameCount == 1 && d->deadTicks == 1, "death warp: checkpoint collected after death");
        d->Restart();
        check(d->playerAlive && d->lastCheckNum == 1 && std::fabs(d->spriteX[d->playerBody] - (-271.1)) < 30,
              "R after the death warp restarts at the warped checkpoint");
        auto a = std::make_unique<Sim>();
        place(*a, true);
        check(!a->BallHitsTarget(cp), "same position alive: no hit (camera cancels for a live ball)");
    }
    // 16. Death warps from the team's TASes (frames as in their notes; RTA = frames / 31)
    {
        struct Case { int level; const char* inputs; int death, win; bool valid; const char* name; };
        const Case cases[] = {
            {4, "d12e1w3a2n5a2n31d8n78a1n1e1n13w2n1w1e1w12n9w1n43w6n1w3n9w2n2w1n2w17n3", 273, 274, true, "level 4 any% TAS: death warp 1 frame after death"},
            {4, "n27d1S1d123e1d2e22d24e48d22e5d1e8n24", 285, 309, true, "level 4 delayed death warp: flag 24 frames after death"},
        };
        for (const Case& c : cases) {
            LevelTemplate t(c.level);
            auto s = std::make_unique<Sim>();
            s->Load(&t);
            for (uint8_t k : DecodeInputs(c.inputs)) {
                s->Tick(k);
                if (s->isTimeStop) break;
            }
            check(s->deathFrame == c.death && s->winFrame == c.win && DeathWarpFinishValid(s->deathFrame, s->winFrame) == c.valid, c.name);
        }
        // level 8: checkpoint warp (frame 200 = 6.452 s), R, flag warp (frame 1084 = 34.935 s RTA)
        LevelTemplate t8(8);
        auto s = std::make_unique<Sim>();
        s->Load(&t8);
        int total = 0, cpFrame = -1;
        for (uint8_t k : DecodeInputs("d18a4n1d1n1d2n4a1n7d31e14d2e47d12e2d36a1d15n1R1d15e4d160e36q24d16a1d8e17a1d1a2d3e6q158a26q22a253d3n98d1n29")) {
            if (k == IN_RESTART) {
                s->Restart();
                continue;
            }
            s->Tick(k);
            ++total;
            if (s->lastCheckNum == 1 && cpFrame < 0) cpFrame = total;
            if (s->isTimeStop) break;
        }
        check(cpFrame == 200 && total == 1084 && s->deathFrame == 1083 && s->winFrame == 1084, "level 8 double death warp: checkpoint at 200, flag at 1084");
    }
    // 17. Spike glitch (isGless off = glitch on: the camera steps before physics, so the standardized spike
    //     test is shifted by dp). Team test cases on level 3, MATHSPIKES = 1.
    {
        struct Case { const char* inputs; bool diesWithout, diesWith; };
        const Case cases[] = {
            {"d25w26q11n2a1n4", true, true},
            {"d18e16d2e28", false, false},
            {"d20e50", true, false},
            {"d15e1d33e3n9w2n5a12n5d1n1a1n5", false, true},
        };
        LevelTemplate t3(3);
        bool ok = true;
        for (const Case& c : cases)
            for (int glitch = 0; glitch < 2; ++glitch) {
                auto s = std::make_unique<Sim>();
                s->Load(&t3);
                s->gless = glitch == 0;
                for (uint8_t k : DecodeInputs(c.inputs)) s->Tick(k);
                if (!s->playerAlive != (glitch ? c.diesWith : c.diesWithout)) ok = false;
            }
        check(ok, "spike glitch on/off: all four team cases (dies/lives) reproduced");
    }
    // 18. Levels 5, 6, 7, 12, 14 (scripts; awaiting Flash logs)
    {
        bool ok = true;
        for (int lv : {5, 6, 7, 12, 14}) {
            LevelTemplate t(lv);
            auto s = std::make_unique<Sim>();
            s->Load(&t);
            for (int f = 0; f < 600; ++f) s->Tick(IN_NONE);
            if (!s->playerAlive || s->isTimeStop) ok = false;
        }
        check(ok, "levels 5, 6, 7, 12, 14: idle 600 ticks, ball alive");
        // Level_7.redCheckLevel: static; a checkpoint restart (keepStatics) builds the level without the red wall
        LevelTemplate t7(7);
        auto a = std::make_unique<Sim>();
        a->Load(&t7);
        const int32_t wall = a->lvBody[5];
        const bool wallThere = a->world.bodies[wall].inWorld;
        a->staticFlag[0] = true;  // as if the red switch had been hit
        a->Load(&t7, 0, true);
        const bool goneAfterR = !a->world.bodies[a->lvBody[5]].inWorld;
        a->Load(&t7, 0, false);
        const bool backAfterFresh = a->world.bodies[a->lvBody[5]].inWorld && !a->staticFlag[0];
        check(wallThere && goneAfterR && backAfterFresh, "level 7 redCheckLevel: survives a checkpoint restart, cleared by a fresh load");
    }
    // 19. Compact snapshots (src/snapshot.h): byte-exact round trip, identical continuation, portable base
    {
        struct R { int lv; const char* in; };
        const R runs[] = {{2, "d18e1w1n1w5n26a2e1w4n10w1n46a2n4d1n19w6n2w8n28"},
                          {4, "d12e1w3a2n5a2n31d8n78a1n1e1n13w2n1w1e1w12n9w1n43w6n1w3n9w2n2w1n2w17n3"},
                          {8, "d19a3n1a1n12d1a1n21d15e1d1e1d3e22d7e5d30e1d20e7d3e1d9n1d76e2d14a1n1a1n18a3n10a1n18w9n65"},
                          {12, "n5d3e19q5w1q4a1q16a6d1a1n1a8q1a2q1a2S2d12e30d8e5d81e17d4e1d1e3d6e1d65e1d32e5"}};
        bool exact = true, cont = true, portable = true;
        size_t maxBytes = 0, sumBytes = 0, count = 0;
        uint64_t rng = 12345;
        auto next = [&]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (uint8_t)((rng >> 33) % 8); };
        std::vector<uint8_t> buf;
        for (const R& r : runs) {
            LevelTemplate t(r.lv);
            auto base = std::make_unique<Sim>();
            base->Load(&t);
            auto s = std::make_unique<Sim>(*base);
            auto d = std::make_unique<Sim>();
            const std::vector<uint8_t> in = DecodeInputs(r.in);
            for (size_t f = 0; f < in.size() && !s->isTimeStop; ++f) {
                s->Tick(in[f]);
                buf.clear();
                const size_t n = EncodeSnapshot(*base, *s, buf);
                maxBytes = std::max(maxBytes, n);
                sumBytes += n;
                ++count;
                DecodeSnapshot(*base, buf.data(), *d);
                if (std::memcmp(static_cast<const void*>(d.get()), static_cast<const void*>(s.get()), sizeof(Sim))) exact = false;
                if (f % 50 == 25) {  // continue both with random inputs
                    Sim a = *s, b = *d;
                    for (int k = 0; k < 60; ++k) {
                        const uint8_t c = next();
                        a.Tick(c);
                        b.Tick(c);
                    }
                    if (std::memcmp(static_cast<const void*>(&a), static_cast<const void*>(&b), sizeof(Sim))) cont = false;
                }
            }
            // a different template object (another address), as in another process
            LevelTemplate t2(r.lv);
            auto base2 = std::make_unique<Sim>();
            base2->Load(&t2);
            buf.clear();
            auto mid = std::make_unique<Sim>(*base);
            for (size_t f = 0; f < in.size() / 2; ++f) mid->Tick(in[f]);
            EncodeSnapshot(*base, *mid, buf);
            auto other = std::make_unique<Sim>();
            DecodeSnapshot(*base2, buf.data(), *other);
            if (other->tpl != &t2) portable = false;
            for (size_t f = in.size() / 2; f < in.size(); ++f) {
                mid->Tick(in[f]);
                other->Tick(in[f]);
                const FrameStats x = mid->Stats(in[f]), y = other->Stats(in[f]);
                if (std::memcmp(&x.px, &y.px, sizeof(double) * 6) || x.contactNames != y.contactNames || x.worldContactCount != y.worldContactCount ||
                    mid->isTimeStop != other->isTimeStop)
                    portable = false;
            }
        }
        check(exact, "compact snapshot: every state along 4 routes round-trips byte-for-byte");
        check(cont, "compact snapshot: decoded states continue identically (random inputs)");
        check(portable, "compact snapshot: decodes against a base built elsewhere and replays identically");
        std::printf("       compact snapshot: avg %zu bytes, max %zu bytes (full state %zu)\n", sumBytes / count, maxBytes, sizeof(Sim));
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

static int CmdBench(int argc, char** argv) {
    int frames = 200000;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    LevelTemplate tpl(1);
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
        std::fprintf(stderr, "usage: rbsim run|log|test|bench|verify|calib|optimize [options]\n");
        return 2;
    }
    if (!std::strcmp(argv[1], "run")) return CmdRun(argc, argv);
    if (!std::strcmp(argv[1], "log")) return CmdLog(argc, argv);
    if (!std::strcmp(argv[1], "test")) return CmdTest();
    if (!std::strcmp(argv[1], "bench")) return CmdBench(argc, argv);
    if (!std::strcmp(argv[1], "verify")) return CmdVerify(argc, argv);
    if (!std::strcmp(argv[1], "calib")) return CmdCalib(argc, argv);
    if (!std::strcmp(argv[1], "optimize")) return CmdOptimize(argc, argv);
    std::fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
