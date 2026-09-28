// redball.h - Red Ball 1 game logic on top of the Box2D port.
// Mirrors Levels/Level.as, Levels/Level_N.as, PlayerBox.as and
// Game.UpdateHandler from the practice-hack SWF.
#pragma once
#include "b2world.h"
#include <string>
#include <vector>

namespace rb {

// Game constants (Levels/Level.as)
constexpr int32_t LEVEL_ITERATIONS = 10;          // Level.m_iterations
constexpr double LEVEL_TIMESTEP = 1.0 / 30.0;     // Level.m_timeStep (== 0.03333333333333333 literal)
constexpr double PHYS_SCALE = 30;                 // Level.m_physScale
constexpr double DEFAULT_DENSITY = 1;
constexpr double DEFAULT_FRICTION = 1;
constexpr double DEFAULT_RESTITUTION = 0.2;
constexpr int32_t PLAYER_SPRITE_WIDTH_TWIPS = 420; // PlayerBox symbol bounds: 21.0 px

// Input codes: 4*Left + 2*Up + 1*Right; 8 = restart from checkpoint ("R")
enum : uint8_t { IN_NONE = 0, IN_R = 1, IN_U = 2, IN_UR = 3, IN_L = 4, IN_LR = 5, IN_LU = 6, IN_LUR = 7,
                 IN_RESTART = 8 };

// A display-list placement taken from the SWF timeline (PlaceObject matrix).
struct Placement {
    int32_t txTwips = 0, tyTwips = 0;  // translation in twips
    double rotationDeg = 0;            // DisplayObject.rotation (0 for all Level 1 bodies)
    double X() const { return txTwips / 20.0; }
    double Y() const { return tyTwips / 20.0; }
};

// Static description of one CreateBody(...) call in a level constructor.
struct BodySpec {
    std::string name;
    Placement placement;
    std::string kind;  // "Polygon" | "Circle"
    double density = 0, friction = 0, restitution = 0;
    std::vector<std::vector<std::pair<double, double>>> polys;  // pixel-space vertices
    double circleSize = 0;                                       // "Circle": diameter in px
};

struct LevelSpec {
    int32_t id = 0;
    std::vector<Placement> checkpoints;  // checkPoint0..N
    std::vector<BodySpec> bodies;        // in constructor order
    double deathY = 0;                   // Level_N.Update: playerBox.y > deathY
};

LevelSpec MakeLevel1();

// Immutable per-level data shared by every simulation instance (and snapshot).
struct LevelTemplate {
    LevelSpec spec;
    GeomTable geoms;
    int32_t playerGeom = -1;
    std::vector<std::vector<int32_t>> bodyGeoms;  // per BodySpec, per polygon
    explicit LevelTemplate(const LevelSpec& s);
};

struct FrameStats {
    int32_t frame;
    uint8_t input;
    double px, py, vx, vy, angle, omega;
    bool probeCenter, probeLeft, probeRight;
    int32_t contactCount;
    bool alive;
    bool sleeping;
};

// Complete, copyable game state (World + game-side fields).
struct Sim {
    const LevelTemplate* tpl = nullptr;
    World world;
    int32_t playerBody = -1;
    bool playerAlive = true;
    bool isTimeStop = false;
    int32_t lastCheckNum = 0;
    int32_t frameCount = 0;
    double playerSpriteX = 0, playerSpriteY = 0;  // display coordinates (twip-quantised)
    // last-frame diagnostics
    bool probeC = false, probeL = false, probeR = false;

    void Load(const LevelTemplate* t);       // Game.SetLevel(id) -> new Level_N()
    void Restart();                          // "R": SetLevel(id, true)
    void Tick(uint8_t input);                // one Game.UpdateHandler iteration
    FrameStats Stats(uint8_t input) const;

   private:
    int32_t CreateLevelBody(const BodySpec& spec, const std::vector<int32_t>& geoms);
    int32_t GetBodyAtPoint(double x, double y, bool includeStatic);
    void LevelUpdate(bool left, bool up, bool right);
};

// Game.as DecodeInRLE / EncodeOutRLE (TAS string format)
std::vector<uint8_t> DecodeInputs(const std::string& s);
std::string EncodeInputsRLE(const std::vector<uint8_t>& in);

}  // namespace rb
