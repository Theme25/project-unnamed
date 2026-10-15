// viewer/tas.h - the TAS session behind rbview: the Practice Hack's TAS mode (Game.as) on top of the bit-exact
// simulator, without any Windows code (tested on Linux by viewer/tas_test.cpp).
//
// Mod terms -> here:
//   gameState (inputs played so far) + saveState[0] (inputs still to play back)  ->  Route::in, split at Session::pos
//   tPause / gameSpeed / isRealTime / RTGS                                      ->  paused / speed / realtimeLoad / fastPlayback
//   saveState[1..9], digits / Shift+digits                                      ->  slots[0..9], SaveSlot / LoadSlot
//   Preloader.mySo.data.backup (routes lost by R, rewinds, loads)               ->  backups
//   isInvincible (G), isGless (Shift+G)                                         ->  Route::invincible, Route::gless
//   "/set -vx -vy -vr -px -py"                                                  ->  Route::warps (kept in the route, replayable)
//   BruteForcer + B / Shift+B                                                   ->  BruteEnv
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../src/redball.h"

namespace tas {

struct WarpParams {
    double vx = 0, vy = 0, vr = 0, px = 0, py = 0;  // m/s, HUD rotation units (0.35 * rad/s), metres
    bool operator==(const WarpParams& o) const { return vx == o.vx && vy == o.vy && vr == o.vr && px == o.px && py == o.py; }
};

// Everything needed to replay an attempt: what the mod keeps in a savestate, plus the settings that change it.
struct Route {
    int level = 1, checkpoint = 0;
    bool gless = false, invincible = false;
    std::vector<uint8_t> in;          // rb::IN_* codes, IN_RESTART (8) = F (checkpoint restart)
    std::map<int, WarpParams> warps;  // warp applied to the state after `index` inputs
    bool operator==(const Route& o) const {
        return level == o.level && checkpoint == o.checkpoint && gless == o.gless && invincible == o.invincible && in == o.in &&
               warps == o.warps;
    }
    bool operator!=(const Route& o) const { return !(*this == o); }
    // a route the real game cannot reproduce (invincibility or warps)
    bool Tool() const { return invincible || !warps.empty(); }
};

// One line of text: "L3 C0 G0 I0 d18e1d58 W12:3,-2,0,10.5,8" (RLE may be empty; warps optional).
std::string RouteToLine(const Route& r);
bool RouteFromLine(const std::string& line, Route& r, std::string& err);
// The game's RLE string (n d w e a S q W + counts, R = restart). Accepts spaces; rejects anything else.
bool ParseRLE(const std::string& raw, std::vector<uint8_t>& out, std::string& err);

struct FrameInfo {
    double bx = 0, by = 0, brot = 0;  // ball sprite (px, degrees written)
    bool alive = true, won = false;
    int frame = 0;  // Game.frameCount
    int lastCheck = 0;
};
enum class EventKind { Death, Checkpoint, Switch, Static, Flag, Restart, Warp };
struct Event {
    int index;  // inputs applied when it happened
    EventKind kind;
    int frame;  // Game.frameCount
    std::string text;
};

// A route simulated once, with a snapshot every K inputs, so any position can be shown at once.
class Timeline {
   public:
    static constexpr int K = 16;
    void Build(const Route& r);  // from scratch
    void Truncate(int n);        // keep the first n inputs
    void Append(uint8_t code);   // one more input at the end
    void SetWarp(int index, const WarpParams& w);  // and re-simulate
    void Advance(rb::Sim& s, int index) const;     // apply input `index` (and the warp after it) to s
    std::unique_ptr<rb::Sim> StateAt(int pos) const;
    const rb::Sim& Tail() const { return *tail_; }
    const Route& route() const { return route_; }
    int size() const { return (int)route_.in.size(); }
    const std::vector<FrameInfo>& info() const { return info_; }  // info()[i]: after i inputs
    const std::vector<Event>& events() const { return events_; }
    int deathFrame() const;  // last death (frameCount), -1
    int winFrame() const;    // flag frame, -1
    int winIndex() const;    // inputs applied when the flag was taken, -1
    bool warpValid() const;  // a death-warp finish is usable in a real run
    rb::LevelTemplate* tpl() const { return tpl_.get(); }

   private:
    struct Key {
        int deathFrame, lastCheck, switchFrame[4];
        bool staticFlag[4], timeStop;
    };
    static Key KeyOf(const rb::Sim& s);
    void Record(const Key& before, const rb::Sim& after, int index);
    Route route_;
    std::shared_ptr<rb::LevelTemplate> tpl_;
    int tplLevel_ = 0;
    std::vector<std::unique_ptr<rb::Sim>> snaps_;  // snaps_[j]: after j*K inputs
    std::unique_ptr<rb::Sim> tail_;                // after all inputs
    std::vector<FrameInfo> info_;
    std::vector<Event> events_;
};

FrameInfo InfoOf(const rb::Sim& s);
uint8_t KeysToCode(bool left, bool up, bool right);

struct NamedState {
    std::string name;
    Route route;
};

// The mod's jump brute-forcer (BruteForcer.as): B marks the take-off, Shift+B the landing frame; the tool lists the
// up-input patterns (same left/right inputs) that reach heights in steps of yOffset, with their predicted end speed.
// rbview also simulates every candidate and shows the real result.
struct BruteCandidate {
    std::vector<uint8_t> in;  // full route inputs
    double predictedVy = 0;   // the mod's estimate
    double endY = 0, endVy = 0;  // simulated at the landing frame (m, m/s)
    bool alive = true;
};
class BruteEnv {
   public:
    bool started = false, active = false;
    int jumpstart = 0, x = 0, currYOffsets = 0, choice = 0;
    double ypos0 = 0, vy0 = 0, maxypos = 0, yOffset = 4 / (441 * 3.141592653589793);
    std::vector<uint8_t> untilJump, lrDuringJump;
    std::vector<BruteCandidate> cands;
    double TargetY() const { return maxypos - currYOffsets * yOffset; }  // metres (the mod moves the ball sprite there)
    // B: `prefix` = inputs so far, `next` = the state one frame on with the keys held now
    void Start(const std::vector<uint8_t>& prefix, const rb::Sim& next);
    // Shift+B: `full` = inputs so far, `now` = the current state
    void End(const std::vector<uint8_t>& full, const rb::Sim& now);
    void Adjust(int change, const Timeline& base);  // W/S (Up/Down): one height step, recompute the candidates
    void Compute(const Timeline& base);             // the mod's getYOffset + a simulation of each candidate
    void Leave() { active = false; }
};

class Session {
   public:
    Timeline tl;     // the route (level settings + all inputs, played and still to play), simulated
    const Route& route() const { return tl.route(); }
    int pos = 0;     // inputs applied to the shown state
    std::unique_ptr<rb::Sim> cur;

    bool paused = false;       // tPause == 0 (T)
    double speed = 1;          // gameSpeed: 0, 1/32 .. 64 (, and .)
    bool realtimeLoad = false; // isRealTime (U): a loaded state plays back in real time instead of at once
    bool fastPlayback = false; // RTGS (Shift+U): playback runs 64 frames per frame
    bool stopAtEnd = false;    // rbview: pause when a playback reaches the end of the route (the mod keeps running)
    double tickAcc = 0;        // gameTick

    Route slots[10];
    bool slotUsed[10] = {};
    std::vector<NamedState> named;
    std::deque<Route> backups;  // newest first, at most 500 (the mod keeps 500)
    BruteEnv brute;
    std::string feedback;  // ioFeedback

    void Load(const Route& r, int newPos);  // replace the route, show the state after newPos inputs
    void Seek(int p);                       // rbview: look at any frame (nothing is changed)
    bool InPlayback() const { return pos < tl.size(); }
    Route Prefix() const;  // the route up to the shown state (what the mod's digit keys save)
    // One Game.UpdateHandler iteration: plays the route's next input, or records `keys` at the end.
    void Step(uint8_t keys);
    // Advance the clock by `frames` (31 fps * elapsed seconds); runs ticks while not paused. Returns ticks run.
    int Run(double frames, uint8_t keys, int maxTicks = 4096);
    void FrameAdvance(uint8_t keys);  // Space while paused / speed 0
    void SpeedUp();                   // .
    void SpeedDown();                 // ,
    void TogglePause();               // T
    void DropPlayback();              // Esc: the rest of the route is discarded
    void UndoFrame();                 // Shift+Space: last frame removed
    void Rewind(int frames);          // Backspace (10) / Shift+Backspace (31)
    void RestartLevel();              // R (also Ctrl+A/D with another level)
    void CheckpointRestart();         // F
    void SaveSlot(int n);
    void LoadSlot(int n);  // Shift+digit
    void ImportSlot(int n, const Route& r, bool load);
    void SaveNamed(const std::string& name);
    void LoadRoute(const Route& r);  // a named state, a slot, a pasted string: instant, or real time with realtimeLoad
    void SetFlags(bool gless, bool invincible);
    void AddWarp(const WarpParams& w);  // "/set" at the current position
    void Backup();                      // keep the current route in the backups (before it is changed)

    // B / Shift+B / env keys
    void BruteStart(uint8_t keys);
    void BruteEnd();
    void BruteApply();

    // persistence: slots, named states and backups (one text file)
    bool SaveStatesFile(const std::string& path) const;
    bool LoadStatesFile(const std::string& path);

   private:
    void TruncateAt(int n);  // drop inputs from n on (backed up), show the end
};

// The mod's command line ("/config ...", "/set ...", "/remap ..."); remaps are returned for the UI to apply.
struct CommandResult {
    std::string feedback;
    bool remapReset = false;
    int remapFrom = -1, remapTo = -1;  // Windows virtual-key codes
    bool displayChanged = false;
};
CommandResult RunCommand(Session& s, const std::string& line, int& decimals);
// the result also says whether the spike rule changed (the UI then re-simulates and updates its controls)
int KeyNameToVk(const std::string& name);  // "A", "SPACE", "LEFT", "65", ...
std::string VkName(int vk);

}  // namespace tas
