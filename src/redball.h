// redball.h - Red Ball 1 game logic on top of the Box2D port.
// Mirrors Levels/Level.as (base), Levels/Level_N.as (per-level constructor and
// Update), PlayerBox.as and Game.UpdateHandler from the practice-hack SWF.
#pragma once
#include "b2world.h"
#include "levels_data.h"
#include "display_data.h"
#include "flash_sintab.h"
#include <map>
#include <string>
#include <vector>

namespace rb {

// Game constants (Levels/Level.as)
constexpr int32_t LEVEL_ITERATIONS = 10;       // Level.m_iterations
constexpr double LEVEL_TIMESTEP = 1.0 / 30.0;  // Level.m_timeStep (== 0.03333333333333333 literal)
constexpr double PHYS_SCALE = 30;              // Level.m_physScale
constexpr double DEFAULT_DENSITY = 1;
constexpr double DEFAULT_FRICTION = 1;
constexpr double DEFAULT_RESTITUTION = 0.2;
constexpr int32_t PLAYER_SPRITE_WIDTH_TWIPS = 420;  // PlayerBox symbol bounds: 21.0 px
constexpr int32_t NUM_LEVELS = 17;

// Input codes: 4*Left + 2*Up + 1*Right; 8 = restart from checkpoint ("R")
enum : uint8_t { IN_NONE = 0, IN_R = 1, IN_U = 2, IN_UR = 3, IN_L = 4, IN_LR = 5, IN_LU = 6, IN_LUR = 7,
                 IN_RESTART = 8 };

// Pixel-space polygon list, as passed to Level.CreateBody(... , [[[x,y],...],...])
using PolyList = std::vector<std::vector<std::pair<double, double>>>;

// ---- display layer: hitTestObject on twip bounding boxes -------------------
// Rectangles are in Level-local twips (Level.x/y are integral twips and the
// camera scale is always 1 -- scaleTimer is never started -- so the global
// offset cancels in any pairwise overlap test).
struct Rect { double x0, y0, x1, y1; };
// Flash's rotation -> 16.16 display matrix and the resulting getBounds (docs/STATS_LOGGING.md 3.6/3.7).
struct FlashMatrix { int32_t a, b; };  // c = -b, d = a (16.16)
// rotationDeg = the value WRITTEN to DisplayObject.rotation (may lie outside (-180, 180]).
FlashMatrix FlashRotationMatrix(double rotationDeg);
// Flash-reported DisplayObject.rotation of rotated timeline clips (docs/STATS_LOGGING.md 3.9).
bool LookupTimelineRotation(int32_t level, const char* name, double& out, bool* measured = nullptr);
extern int32_t g_provisionalRotations;  // uses of provisional (unmeasured) timeline rotations
struct DisplayConfig {
    bool inclusive = true;  // touching edges count as a hit (VERIFIED: E2 calibration, 5/5 touching cases hit)
};
DisplayConfig& GetDisplayConfig();
// adj = -1/0/+1 on the matrix entries (in 1/65536 units): the fitted matrix is exact on 14,496 of
// 14,500 logged matrices; the 4 misses are near-ties in the last rounding step. Hit tests are also made
// with the matrix nudged by -1/+1 and disagreements are flagged in Sim::displayUncertain.
Rect BallBounds(double spriteXpx, double spriteYpx, double rotationDeg, int adj = 0);
bool RectsHit(const Rect& a, const Rect& b);
// Standardized spikes (Practice Hack CONFIG.MATHSPIKES = 1, the speedrun rule):
// PlayerBox.HitTestObjectControlPoints -> Shipik.testPoint for 16 control points
// (10.5 px radius, through the ball's display matrix). Each point, in Level space, must lie in the
// Shipik's getBounds rectangle; then it is shifted by -dp (the camera step of this frame) and tested
// strictly against the triangle (0,0) (3,-9.65) (6,0), with Flash's twip conversions reproduced exactly
// (docs/STATS_LOGGING.md 3.8). Rotated/scaled Shipiks (later levels) are not calibrated yet: decisions
// within SPIKE_EDGE_MARGIN twips of an edge there are counted in Sim::displayUncertain.
constexpr double SPIKE_EDGE_MARGIN = 1.0;
struct SpikeResult { bool hit; bool uncertain; };
SpikeResult BallHitsSpike(double spriteXpx, double spriteYpx, double rotationDeg, double dpx, double dpy, const SpikeObj& s);

// Real-game timers (not in the TAS hack; teammate data, 31 fps): after a death the game respawns 1.2 s later
// and after the flag it enters the next level 2.839 s later; both timers run while paused, the actions only
// happen unpaused. A death-warp finish counts only if the game is paused before the respawn fires, i.e. the
// flag frame must be <= death frame + RESPAWN_LAST_PAUSE_FRAMES; the optimal unpause is flag + 88.
// Checked against both Level 4 examples (pause windows 274-311 and 309-323, unpauses 362 and 397).
constexpr int32_t RESPAWN_LAST_PAUSE_FRAMES = 38;  // 1.2 s * 31 fps -> last pausable frame = death + 38
constexpr int32_t WIN_TIMER_FRAMES = 88;           // 2.839 s * 31 fps
// For a death-warp finish: whether the warp is usable in a real run (flag hit before the respawn).
inline bool DeathWarpFinishValid(int32_t deathFrame, int32_t winFrame) {
    return deathFrame < 0 || winFrame <= deathFrame + RESPAWN_LAST_PAUSE_FRAMES;
}

struct Sim;
struct LevelScript {
    int32_t id;
    void (*construct)(Sim&);  // body of Level_N() after super()
    void (*update)(Sim&);     // body of Level_N.Update() after super.Update()
    bool implemented;
    // The part of Level_N.Update() that still matters after death: unguarded hitTestObject switches
    // (dead-ball rule) and their persistent effects. nullptr: nothing.
    void (*deadUpdate)(Sim&) = nullptr;
};
const LevelScript& GetLevelScript(int32_t id);

// Immutable per-level data shared by every simulation instance (and snapshot).
// Geometry is registered on first construction and reused afterwards.
struct LevelTemplate {
    int32_t id = 0;
    const LevelPlacements* placements = nullptr;
    GeomTable geoms;
    int32_t playerGeom = -1;
    std::map<std::string, int32_t> geomCache;  // "name#i" -> geom id
    std::vector<std::string> bodyNames;        // CreateBody order (userTag = 100 + index)
    bool frozen = false;                       // no new geometry after the first load
    explicit LevelTemplate(int32_t levelId);
    const RawPlacement& Place(const char* name) const;
    const DisplayObj* Display(const char* name) const;  // nullptr if the level has no such object
    const DisplayObj* aim = nullptr;                    // levelAim
    const SpikeObj* spikes = nullptr;                   // every top-level Ships10/Shipik spike
    int32_t spikeCount = 0;
    const DisplayObj* cps[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};  // checkPoint0..4
    bool HasPlacement(const char* name) const;
    int32_t CheckpointCount() const;
};

struct FrameStats {
    int32_t frame;
    uint8_t input;
    double px, py, vx, vy, angle, omega;
    bool probeCenter, probeLeft, probeRight;
    int32_t contactCount;
    bool alive;
    bool sleeping;
    double sleepTime;
    uint32_t flags;
    int32_t worldContactCount;
    std::string contactNames;  // playerContactBodies names, comma-joined
    double sx, sy, sr;         // PlayerBox sprite x/y/rotation
    bool timeStop;             // Level.isTimeStop (win)
};

constexpr int32_t LV_VARS = 8;

// Complete, copyable game state (World + game-side fields).
struct Sim {
    LevelTemplate* tpl = nullptr;
    World world;
    int32_t playerBody = -1;
    bool playerAlive = true;
    bool isTimeStop = false;
    int32_t lastCheckNum = 0;  // Level.lastCheckNum (static in AS3: survives restarts)
    int32_t displayUncertain = 0;  // # of goal/checkpoint tests that flip if a matrix entry is off by one unit
    int32_t aimFrame = 1;      // levelAim.currentFrame (1 = armed)
    // Camera = Level.x/y, moved by one Tweener step (easeOutExpo, 31 frames, t = 1) per Update.
    double camX = 0, camY = 0;          // Level.x / Level.y (whole twips)
    double camTargetX = 0, camTargetY = 0;
    bool camTween = false;              // a camera tween was added by the previous Update
    double dpX = 0, dpY = 0;            // Level.dp: camera step of this Update (old - new)
    int32_t deadTicks = 0;             // Updates run since the ball died
    // AS3 static level flags: Level_7.redCheckLevel (later: Level_13.greenCheckLevel, Level_16.isStrelka).
    // Cleared by a fresh SetLevel, kept by SetLevel(id, true).
    bool staticFlag[4] = {false, false, false, false};
    int32_t switchFrame[4] = {1, 1, 1, 1};  // level switch clips (blueCheck/greenCheck/redCheck) currentFrame
    int32_t deathFrame = -1;           // frameCount when PlayerDie ran (-1: alive)
    int32_t winFrame = -1;             // frameCount of the Update that called PlayerWin
    double winMargin = 0;              // overlap depth (twips, min of x/y overlap) of ball and flag at the win
    bool gless = false;                 // Game.isGless: camera steps at the end of Update, dp = 0
    int32_t cpFrame[5] = {1, 1, 1, 1, 1};  // checkPointN.currentFrame (1 = armed, 5 = already collected)
    int32_t frameCount = 0;
    // display layer: DisplayObject x/y/rotation of each body's sprite (twip-quantised)
    double spriteX[CAP_BODIES], spriteY[CAP_BODIES], spriteRot[CAP_BODIES];
    // The value last written to DisplayObject.rotation (before Flash's normalisation). The display
    // matrix is built from THIS value (FlashRotationMatrix), not from the normalised getter value.
    double spriteRotW[CAP_BODIES];
    bool hasSprite[CAP_BODIES];
    // per-level script state (Level_N private fields)
    int32_t lvBody[LV_VARS];
    int32_t lvInt[LV_VARS];
    // last-frame diagnostics
    bool probeC = false, probeL = false, probeR = false;

    // keepStatics: AS3 static level flags survive (R / F restart via SetLevel(id, true) and log segments
    // that follow one); a fresh level load clears them.
    void Load(LevelTemplate* t, int32_t checkpoint = 0, bool keepStatics = false);  // Game.SetLevel(id) -> new Level_N()
    void Restart();                                       // "R": SetLevel(id, true)
    void Tick(uint8_t input);                             // one Game.UpdateHandler iteration
    FrameStats Stats(uint8_t input) const;
    std::string BodyName(int32_t body) const;

    // API used by level scripts (mirrors Level.as helpers)
    int32_t CreateBody(const char* name, const char* kind, double density, double friction, double restitution,
                       const PolyList& polys);
    int32_t CreateCircleBody(const char* name, double density, double friction, double restitution, double size);
    void PlayerDie();
    void PlayerWin();
    // flags column of the v2 log block: bit0 Level_7.redCheckLevel, bit1 Level_13.greenCheckLevel,
    // bit2 Level_16.isStrelka; bit8 blueCheck, bit9 greenCheck, bit10 redCheck taken (currentFrame != 1)
    int32_t LoggedFlags() const;
    int32_t createAtSprite = -1;  // >= 0: next CreateBody uses this body's sprite state (AS3 CreateBody on a moved clip)
    bool BallHitsTarget(const DisplayObj& o);  // hitTestObject, alive or dead (death-warp rule)
    double TargetOverlap(const DisplayObj& o) const;  // min(x, y) overlap of the boxes in twips (< 0: apart)
    double SpriteX(int32_t body) const { return spriteX[body]; }
    double SpriteY(int32_t body) const { return spriteY[body]; }

   private:
    int32_t GetBodyAtPoint(double x, double y, bool includeStatic);
    void LevelUpdate(bool left, bool up, bool right);
    void DisplayUpdate();
    void DeadUpdate();     // Level.Update after PlayerDie (camera + goal/checkpoint tests only)
    void CameraStep();     // Tweener.onEnterFrame via COMM "TweenEvent"  // win check + checkpoints (Level.Update, after the input forces)
    int32_t Geom(const std::string& key, const ShapeDef& def);
    int32_t BeginBody(const char* name);
};

// Game.as DecodeInRLE / EncodeOutRLE (TAS string format)
std::vector<uint8_t> DecodeInputs(const std::string& s);
std::string EncodeInputsRLE(const std::vector<uint8_t>& in);

}  // namespace rb
