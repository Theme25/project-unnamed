// viewer/tas_test.cpp - checks of the TAS session core (tas.cpp) on any platform: make tastest && ./viewer/tas_test
#include "tas.h"

#include <cstdio>
#include <cstring>
#include <random>

using namespace rb;
using namespace tas;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++failures;
}

static bool Same(const Sim& a, const Sim& b) {
    const Body& x = a.world.bodies[a.playerBody];
    const Body& y = b.world.bodies[b.playerBody];
    return a.frameCount == b.frameCount && a.playerAlive == b.playerAlive && a.isTimeStop == b.isTimeStop && a.camX == b.camX &&
           a.camY == b.camY && std::memcmp(&x.xf, &y.xf, sizeof x.xf) == 0 && std::memcmp(&x.linearVelocity, &y.linearVelocity, sizeof x.linearVelocity) == 0 &&
           x.angularVelocity == y.angularVelocity && a.lastCheckNum == b.lastCheckNum && a.deathFrame == b.deathFrame;
}

static std::unique_ptr<Sim> Direct(const Route& r, int n) {
    auto t = std::make_unique<LevelTemplate>(r.level);
    static std::vector<std::unique_ptr<LevelTemplate>> keep;
    auto s = std::make_unique<Sim>();
    s->Load(t.get(), r.checkpoint);
    s->gless = r.gless;
    s->invincible = r.invincible;
    for (int i = 0; i < n; ++i) {
        if (r.in[(size_t)i] == IN_RESTART) s->Restart();
        else s->Tick(r.in[(size_t)i]);
    }
    keep.push_back(std::move(t));
    return s;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* tasL2 = "d18e1w1n1w5n26a2e1w4n10w1n46a2n4d1n19w6n2w8n28";
    // 1. recording from the keyboard gives the same states as replaying the string
    {
        Session s;
        Route r;
        r.level = 2;
        s.Load(r, 0);
        const std::vector<uint8_t> in = DecodeInputs(tasL2);
        for (uint8_t c : in) s.Step(c);
        Route full = r;
        full.in = in;
        auto d = Direct(full, (int)in.size());
        check(s.route().in == in && s.pos == (int)in.size() && Same(*s.cur, *d), "recording = replaying the route string (Level 2 TAS)");
        check(s.tl.winFrame() == 186, "the Level 2 TAS takes the flag on frame 186");
        // inputs after the flag are not recorded (the game stops counting)
        s.Step(IN_R);
        check((int)s.route().in.size() <= (int)in.size(), "no inputs recorded after the flag");
    }
    // 2. seeking anywhere = direct simulation; truncate + append = rebuild
    {
        Session s;
        Route r;
        r.level = 8;
        r.in = DecodeInputs("d19a3n1a1n12d1a1n21d15e1d1e1d3e22d7e5d30e1d20e7d3e1d9n1d76e2d14a1n1a1n18a3n10a1n18w9n65");
        s.Load(r, 0);
        std::mt19937 rng(5);
        bool ok = true;
        for (int k = 0; k < 20; ++k) {
            const int p = (int)(rng() % (r.in.size() + 1));
            s.Seek(p);
            ok = ok && Same(*s.cur, *Direct(r, p));
        }
        check(ok, "seek: 20 random positions on Level 8 match a direct simulation");
        s.Seek(100);
        s.paused = true;
        s.Rewind(10);  // Backspace
        Route cut = r;
        cut.in.resize(90);
        check(s.pos == 90 && s.route().in == cut.in && Same(*s.cur, *Direct(cut, 90)) && !s.backups.empty() && s.backups.front() == r,
              "Backspace: 10 frames removed, the old route backed up");
        for (int i = 0; i < 30; ++i) s.Step(r.in[(size_t)(90 + i)]);
        Timeline fresh;
        Route r120 = r;
        r120.in.resize(120);
        fresh.Build(r120);
        bool evOk = fresh.events().size() == s.tl.events().size();
        check(evOk && Same(*s.cur, fresh.Tail()), "re-recording after a rewind = a fresh build (states and events)");
        s.UndoFrame();
        check(s.pos == 119 && s.route().in.size() == 119, "Shift+Space: one frame removed");
    }
    // 3. playback, real-time loads, Esc, F, R, slots
    {
        Session s;
        Route r;
        r.level = 2;
        r.in = DecodeInputs("d30");
        s.Load(r, 0);
        s.realtimeLoad = true;
        s.LoadRoute(r);
        check(s.pos == 0 && !s.paused && s.InPlayback(), "real-time load: playback from the start");
        s.Run(10, IN_L);
        check(s.pos == 10 && s.route().in == r.in, "keys are ignored during playback");
        for (int i = 0; i < 25; ++i) s.Run(1, IN_L);  // one display frame at a time, as the UI does
        check(s.route().in.size() == 35 && s.route().in[30] == IN_L && s.feedback == "Playback Ended", "after the playback the keys are recorded");
        s.SaveSlot(3);
        s.paused = true;
        s.Seek(12);
        s.DropPlayback();
        check(s.route().in.size() == 12 && s.backups.front().in.size() == 35, "Esc drops the rest of the route");
        s.realtimeLoad = false;
        s.LoadSlot(3);
        check(s.pos == 35 && s.route().in.size() == 35 && !s.InPlayback(), "Shift+digit (instant): the slot's route, at its end");
        s.CheckpointRestart();
        check(s.route().in.back() == IN_RESTART && s.pos == (int)s.route().in.size() && s.cur->frameCount == 35, "F appends a restart (frameCount keeps counting)");
        s.RestartLevel();
        check(s.route().in.empty() && s.pos == 0, "R clears the route");
        s.SpeedDown(), s.SpeedDown();
        check(s.speed == 0.25, "speed halves with ,");
        for (int i = 0; i < 9; ++i) s.SpeedDown();
        check(s.speed == 0, "speed reaches 0");
        s.SpeedUp();
        check(s.speed == 1.0 / 32, "and comes back as 1/32");
    }
    // 4. text formats and the states file
    {
        Route r;
        r.level = 9, r.checkpoint = 1, r.gless = true, r.invincible = true;
        r.in = DecodeInputs("d12e1R1n5");
        r.warps[7] = {3.25, -2, 0.1, 12.5, 9.0 / 7};
        Route back;
        std::string err;
        check(RouteFromLine(RouteToLine(r), back, err) && back == r, "route line round trip (flags, restart, warp)");
        Session s;
        s.Load(r, 3);
        s.SaveSlot(1);
        s.SaveNamed("first try");
        s.Backup();
        const std::string path = "/tmp/tas_test_states.txt";
        s.SaveStatesFile(path);
        Session t;
        t.LoadStatesFile(path);
        check(t.slotUsed[1] && t.slots[1] == s.slots[1] && t.named.size() == 1 && t.named[0].name == "first try" && t.backups.size() == 1,
              "states file round trip");
    }
    // 5. invincibility, warps, commands
    {
        Session s;
        Route r;
        r.level = 3;
        r.in = DecodeInputs("d18e1d58n40");
        s.Load(r, (int)r.in.size());
        check(!s.cur->playerAlive, "Level 3 route dies");
        s.SetFlags(false, true);
        check(s.cur->playerAlive && s.route().invincible, "G: same route survives");
        s.SetFlags(false, false);
        s.Seek(30);
        int dec = 4;
        CommandResult c = RunCommand(s, "/set -vx 5 -py 12", dec);
        const Body& b = s.cur->world.bodies[s.cur->playerBody];
        check(c.feedback == "Set 2 parameters." && b.linearVelocity.x == 5 && b.xf.position.y == 12 && s.route().warps.count(30),
              "/set warps the ball at the current frame and keeps it in the route");
        Route saved = s.route();
        Timeline t;
        t.Build(saved);
        check(Same(t.Tail(), s.tl.Tail()), "a route with a warp replays identically");
        c = RunCommand(s, "/config decimals 6", dec);
        check(dec == 6 && c.feedback == "Config Accepted: DECIMALS : 6", "/config decimals");
        c = RunCommand(s, "/remap a left", dec);
        check(c.remapFrom == 'A' && c.remapTo == 37, "/remap a left");
        c = RunCommand(s, "/config mathspikes 2", dec);
        check(c.displayChanged && GetDisplayConfig().mathSpikes == 2, "/config mathspikes 2");
        GetDisplayConfig().mathSpikes = 1;
        c = RunCommand(s, "hello", dec);
        check(c.feedback == "Malformed Configuration Settings", "unknown command");
    }
    // 6. the jump brute-forcer
    {
        Session s;
        Route r;
        r.level = 1;
        r.in = DecodeInputs("n20e1");  // the take-off frame: jump held on the ground
        s.Load(r, 21);
        s.BruteStart(IN_UR);
        for (int i = 0; i < 19; ++i) s.Step(i < 5 ? IN_UR : IN_R);  // up held for 6 frames in all, then right
        const double actualY = s.cur->world.bodies[s.cur->playerBody].xf.position.y;
        s.BruteEnd();
        const BruteEnv& e = s.brute;
        bool shapes = !e.cands.empty();
        for (const BruteCandidate& c : e.cands) shapes = shapes && (int)c.in.size() == 40 && std::equal(c.in.begin(), c.in.begin() + 21, r.in.begin());
        bool own = false;  // the route that was played is one of the candidates at the current height
        for (const BruteCandidate& c : e.cands) own = own || (c.in == s.route().in && c.endY == actualY);
        std::printf("       target %.6f m, played %.6f m\n", e.TargetY(), actualY);
        for (const BruteCandidate& c : e.cands)
            std::printf("       %s  predicted vy %.4f  simulated y %.6f vy %.4f\n", EncodeInputsRLE(c.in).c_str(), c.predictedVy, c.endY, c.endVy);
        check(own, "the played jump is among the candidates, with its real end height");
        check(e.active && shapes, "B / Shift+B: candidates keep the route before the take-off and its length");
        const int before = e.currYOffsets;
        s.brute.Adjust(1, s.tl);
        check(s.brute.currYOffsets == before + 1, "W: one height step");
        if (!s.brute.cands.empty()) {
            s.BruteApply();
            check(s.route().in == s.brute.cands[(size_t)s.brute.choice].in && s.pos == (int)s.route().in.size(), "Enter loads the chosen candidate");
        }
        std::printf("       brute force: %zu candidates, x=%d, offsets %d\n", s.brute.cands.size(), s.brute.x, s.brute.currYOffsets);
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
