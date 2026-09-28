// redball.cpp - game-side logic (Level.as / Level_1.as / PlayerBox.as /
// Game.UpdateHandler) driving the Box2D port.
#include "redball.h"
#include <cstdio>
#include <cmath>

namespace rb {

// ---------------------------------------------------------------- display layer
// DisplayObject.x/y setters store twips (integer 1/20 px).
// UNVERIFIED: truncation toward zero vs round-to-nearest. The TAS stat logs
// (sprite x/y) will settle this; it only matters for display-driven checks
// (death line, hitTestObject for goal/checkpoints).
// VERIFIED (calibration dump, 2400 samples): truncation toward zero.
static double SpriteCoord(double v) { return (double)as3_toInt32(v * 20) / 20.0; }

// DisplayObject.rotation: the getter returns the written value normalised to
// (-180, 180] via fmod (VERIFIED, 720 samples). Level.Update writes
// angle * (180 / Math.PI) % 360.
static double SpriteRotation(double angle) {
    double v = std::fmod(angle * (180 / AS3_PI), 360.0);
    v = std::fmod(v, 360.0);
    if (v > 180) v -= 360;
    else if (v < -180) v += 360;
    return v;
}

// ---------------------------------------------------------------- level data
// Level 1 placements extracted from the SWF (DefineSprite for Level_1,
// PlaceObject matrices, translate in twips).
LevelSpec MakeLevel1() {
    LevelSpec L;
    L.id = 1;
    L.deathY = 550;
    L.checkpoints.push_back(Placement{338, 4986, 0});  // checkPoint0 (16.9, 249.3)
    auto box = [](double w, double h) {
        return std::vector<std::pair<double, double>>{{w, 0}, {w, h}, {0, h}, {0, 0}};
    };
    BodySpec start;
    start.name = "startPlatform";
    start.placement = Placement{-400, 5400, 0};
    start.kind = "Polygon";
    start.density = 0;
    start.friction = DEFAULT_FRICTION;
    start.restitution = DEFAULT_RESTITUTION;
    start.polys = {box(200, 20)};
    L.bodies.push_back(start);

    BodySpec exitp = start;
    exitp.name = "exitPlatform";
    exitp.placement = Placement{4800, 5400, 0};
    exitp.polys = {box(300, 20)};
    L.bodies.push_back(exitp);

    BodySpec barier = start;
    barier.name = "barier";
    barier.placement = Placement{6680, 4420, 0};
    barier.polys = {{{0, 0}, {20, 0}, {20, 50}, {0, 50}}};
    L.bodies.push_back(barier);
    return L;
}

LevelTemplate::LevelTemplate(const LevelSpec& s) : spec(s) {
    // PlayerBox circle: radius = width / m_physScale / 2
    ShapeDef pd;
    pd.type = e_circleShape;
    double width = PLAYER_SPRITE_WIDTH_TWIPS / 20.0;
    pd.radius = width / PHYS_SCALE / 2;
    pd.localPosition.Set(0, 0);
    playerGeom = geoms.Add(pd);
    for (const BodySpec& b : spec.bodies) {
        std::vector<int32_t> gs;
        if (b.kind == "Polygon" || b.kind == "BluePolygon") {
            for (const auto& poly : b.polys) {
                ShapeDef d;
                d.type = e_polygonShape;
                d.vertexCount = (int32_t)poly.size();
                for (int32_t i = 0; i < d.vertexCount; ++i)
                    d.vertices[i].Set(poly[(size_t)i].first / PHYS_SCALE, poly[(size_t)i].second / PHYS_SCALE);
                gs.push_back(geoms.Add(d));
            }
        } else if (b.kind == "Circle") {
            ShapeDef d;
            d.type = e_circleShape;
            d.radius = b.circleSize / PHYS_SCALE / 2;
            d.localPosition.Set(b.circleSize / PHYS_SCALE / 2, b.circleSize / PHYS_SCALE / 2);
            gs.push_back(geoms.Add(d));
        } else {
            fatal("unknown body kind");
        }
        bodyGeoms.push_back(gs);
    }
}

// ---------------------------------------------------------------- construction

int32_t Sim::CreateLevelBody(const BodySpec& spec, const std::vector<int32_t>& gs) {
    BodyDef bd;
    bd.position = Vec2(spec.placement.X() / PHYS_SCALE, spec.placement.Y() / PHYS_SCALE);
    bd.angle = spec.placement.rotationDeg * (AS3_PI / 180);
    bd.userTag = 100 + (int32_t)(&spec - &tpl->spec.bodies[0]);
    int32_t b = world.CreateBody(bd);
    if (spec.kind == "Polygon" || spec.kind == "BluePolygon") {
        for (size_t i = 0; i < gs.size(); ++i) {
            ShapeDef d;
            d.type = e_polygonShape;
            d.density = spec.density;
            d.friction = spec.friction;
            d.restitution = spec.restitution;
            world.CreateShape(b, gs[i], d);
        }
    } else {
        ShapeDef d;
        d.type = e_circleShape;
        d.density = spec.density;
        d.friction = spec.friction;
        d.restitution = spec.restitution;
        world.CreateShape(b, gs[0], d);
    }
    world.SetMassFromShapes(b);
    return b;
}

void Sim::Load(const LevelTemplate* t) {
    tpl = t;
    lastCheckNum = 0;
    frameCount = 0;
    Restart();
}

void Sim::Restart() {
    // --- Level() constructor
    AABB worldAABB;
    worldAABB.lowerBound.Set(-1000, -1000);
    worldAABB.upperBound.Set(1000, 1000);
    world.Init(&tpl->geoms, worldAABB, Vec2(0, 10), true);
    world.listener.enabled = true;  // m_world.SetContactListener(myContactListener)

    // PlayerBox(playerStartPosition.x, playerStartPosition.y, m_world, m_sprite)
    const Placement& cp = tpl->spec.checkpoints[(size_t)lastCheckNum];
    double px = cp.X(), py = cp.Y();
    playerSpriteX = SpriteCoord(px);
    playerSpriteY = SpriteCoord(py);
    playerSpriteRot = 0;
    BodyDef bd;
    bd.position.Set(px / PHYS_SCALE, py / PHYS_SCALE);
    bd.userTag = 0;
    playerBody = world.CreateBody(bd);
    world.bodies[playerBody].flags |= BF_BULLET;  // SetBullet(true)
    ShapeDef sd;
    sd.type = e_circleShape;
    sd.density = 1;
    sd.friction = 0.4;
    sd.restitution = 0.2;
    world.CreateShape(playerBody, tpl->playerGeom, sd);
    world.SetMassFromShapes(playerBody);
    world.listener.playerBody = playerBody;

    // --- Level_N() constructor: CreateBody(...) calls in order
    for (size_t i = 0; i < tpl->spec.bodies.size(); ++i) CreateLevelBody(tpl->spec.bodies[i], tpl->bodyGeoms[i]);

    playerAlive = true;
    isTimeStop = false;
    probeC = probeL = probeR = false;
}

// ---------------------------------------------------------------- per-frame

int32_t Sim::GetBodyAtPoint(double x, double y, bool includeStatic) {
    Vec2 p(x, y);
    AABB aabb;
    aabb.lowerBound.Set(x - 0.001, y - 0.001);
    aabb.upperBound.Set(x + 0.001, y + 0.001);
    int32_t results[10];
    int32_t count = world.Query(aabb, results, 10);
    for (int32_t i = 0; i < count; ++i) {
        int32_t s = results[i];
        int32_t b = world.shapes[s].body;
        if (world.bodies[b].IsStatic() == false || includeStatic) {
            if (world.ShapeTestPoint(s, world.bodies[b].xf, p)) return b;
        }
    }
    return -1;
}

void Sim::LevelUpdate(bool left, bool up, bool right) {
    world.Step(LEVEL_TIMESTEP, LEVEL_ITERATIONS);

    // sprite sync for the player (other dynamic sprites are cosmetic here)
    {
        const Body& pb = world.bodies[playerBody];
        if (!pb.IsStatic()) {
            playerSpriteX = SpriteCoord(pb.xf.position.x * PHYS_SCALE);
            playerSpriteY = SpriteCoord(pb.xf.position.y * PHYS_SCALE);
            playerSpriteRot = SpriteRotation(pb.sweep.a);
        }
    }

    // ground probes (Body object stays readable even after DestroyBody)
    const Vec2 pos = world.bodies[playerBody].xf.position;
    probeC = GetBodyAtPoint(pos.x, pos.y + 0.4, true) != -1;
    const Vec2 pos2 = world.bodies[playerBody].xf.position;
    probeL = GetBodyAtPoint(pos2.x - 0.2, pos2.y + 0.38, true) != -1;
    const Vec2 pos3 = world.bodies[playerBody].xf.position;
    probeR = GetBodyAtPoint(pos3.x + 0.2, pos3.y + 0.38, true) != -1;
    const bool grounded = probeC || probeL || probeR;

    Body& b = world.bodies[playerBody];
    // `_loc7_` aliases m_linearVelocity, so every read below is live.
    if (left) {
        b.WakeUp();
        if (grounded) {
            if (b.linearVelocity.x > -5) b.linearVelocity = Vec2(b.linearVelocity.x - 0.5, b.linearVelocity.y);
        } else if (b.linearVelocity.x > -5.0 / 2) {
            b.linearVelocity = Vec2(b.linearVelocity.x - 0.25, b.linearVelocity.y);
        }
    }
    if (right) {
        b.WakeUp();
        if (grounded) {
            if (b.linearVelocity.x < 5) b.linearVelocity = Vec2(b.linearVelocity.x + 0.5, b.linearVelocity.y);
        } else if (b.linearVelocity.x < 5.0 / 2) {
            b.linearVelocity = Vec2(b.linearVelocity.x + 0.25, b.linearVelocity.y);
        }
    }
    if (up) {
        b.WakeUp();
        if (world.listener.count > 0 && grounded) {
            world.ApplyForce(playerBody, Vec2(0, -65), world.bodies[playerBody].sweep.c);
        } else if (b.linearVelocity.y < 0) {
            world.ApplyForce(playerBody, Vec2(0, -1), world.bodies[playerBody].sweep.c);
        }
    }

    // TODO(display layer): levelAim.hitTestObject -> PlayerWin, spikes,
    // checkpoints. Level_1.Update death line:
    if (playerSpriteY > tpl->spec.deathY && playerAlive) {
        // PlayerDie() spawns Math.random() debris: treat death as terminal.
        playerAlive = false;
    }
}

void Sim::Tick(uint8_t input) {
    // Game.UpdateHandler (playback): Left = v>=4, Up = v>=6||v==2||v==3, Right = v%2==1
    bool left = input >= 4;
    bool up = input >= 6 || input == 2 || input == 3;
    bool right = input % 2 == 1;
    LevelUpdate(left, up, right);
    if (!isTimeStop) ++frameCount;
}

FrameStats Sim::Stats(uint8_t input) const {
    const Body& b = world.bodies[playerBody];
    FrameStats s;
    s.frame = frameCount;
    s.input = input;
    s.px = b.xf.position.x;
    s.py = b.xf.position.y;
    s.vx = b.linearVelocity.x;
    s.vy = b.linearVelocity.y;
    s.angle = b.sweep.a;
    s.omega = b.angularVelocity;
    s.probeCenter = probeC;
    s.probeLeft = probeL;
    s.probeRight = probeR;
    s.contactCount = world.listener.count;
    s.alive = playerAlive;
    s.sleeping = b.IsSleeping();
    s.sleepTime = b.sleepTime;
    s.flags = b.flags;
    s.worldContactCount = world.contactCount;
    for (int32_t i = 0; i < world.listener.count; ++i) {
        if (i) s.contactNames += ",";
        s.contactNames += BodyName(world.listener.bodies[i]);
    }
    s.sx = playerSpriteX;
    s.sy = playerSpriteY;
    s.sr = playerSpriteRot;
    return s;
}

std::string Sim::BodyName(int32_t body) const {
    if (body == world.groundBody) return "ground";
    int32_t tag = world.bodies[body].userTag;
    if (tag >= 100 && tag - 100 < (int32_t)tpl->spec.bodies.size()) return tpl->spec.bodies[(size_t)(tag - 100)].name;
    if (body == playerBody) return "playerBox";
    return "body" + std::to_string(body);
}

// ---------------------------------------------------------------- input codec

std::vector<uint8_t> DecodeInputs(const std::string& s) {
    std::vector<uint8_t> out;
    if (s.empty()) return out;
    if ((unsigned char)s[0] < 64) {  // legacy one-digit-per-frame format
        for (char ch : s) out.push_back((uint8_t)(ch - 48));
        return out;
    }
    int code = 0;
    size_t i = 0;
    while (i < s.size()) {
        switch (s[i]) {
            case 'a': code = 4; break;
            case 'w': code = 2; break;
            case 'd': code = 1; break;
            case 'q': code = 6; break;
            case 'e': code = 3; break;
            case 'S': code = 5; break;
            case 'W': code = 7; break;
            case 'n': code = 0; break;
            case 'R': code = 8; break;
            default: break;  // unknown letters keep the previous code (as in AS3)
        }
        ++i;
        long count = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            count = count * 10 + (s[i] - '0');
            ++i;
        }
        while (count > 0) {
            out.push_back((uint8_t)code);
            if (code != 8)
                --count;
            else
                count = 0;
        }
    }
    return out;
}

std::string EncodeInputsRLE(const std::vector<uint8_t>& in) {
    static const char* letters = "ndweaSqWR";  // index by code 0..8
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        uint8_t c = in[i];
        size_t j = i;
        if (c == 8) {
            j = i + 1;
            out += "R1";
        } else {
            while (j < in.size() && in[j] == c) ++j;
            out += letters[c];
            out += std::to_string(j - i);
        }
        i = j;
    }
    return out;
}

}  // namespace rb
