// redball.cpp - game-side logic (Level.as / Level_N.as / PlayerBox.as /
// Game.UpdateHandler) driving the Box2D port.
#include "redball.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rb {

// ---------------------------------------------------------------- display layer
// DisplayObject.x/y setters store twips: truncation toward zero
// (VERIFIED, calibration dump, 2400 samples on two players).
static double SpriteCoord(double v) { return (double)as3_toInt32(v * 20) / 20.0; }

// DisplayObject.rotation: the getter returns the written value normalised to
// (-180, 180] via fmod (VERIFIED, 720 samples). Level.Update writes
// angle * (180 / Math.PI) % 360.
static double SpriteRotationWritten(double angle) { return std::fmod(angle * (180 / AS3_PI), 360.0); }
static double SpriteRotation(double angle) {
    double v = std::fmod(angle * (180 / AS3_PI), 360.0);
    v = std::fmod(v, 360.0);
    if (v > 180) v -= 360;
    else if (v < -180) v += 360;
    return v;
}

// ---------------------------------------------------------------- templates

LevelTemplate::LevelTemplate(int32_t levelId) : id(levelId) {
    for (const LevelPlacements& lp : kLevelPlacements)
        if (lp.id == levelId) placements = &lp;
    if (!placements) fatal("unknown level id");
    ShapeDef pd;  // PlayerBox circle: radius = width / m_physScale / 2
    pd.type = e_circleShape;
    double width = PLAYER_SPRITE_WIDTH_TWIPS / 20.0;
    pd.radius = width / PHYS_SCALE / 2;
    pd.localPosition.Set(0, 0);
    playerGeom = geoms.Add(pd);
    aim = Display("levelAim");
    for (const LevelSpikes& ls : kSpikeTable)
        if (ls.id == id) {
            spikes = ls.items;
            spikeCount = ls.count;
        }
    for (int32_t i = 0; i < 5; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "checkPoint%d", i);
        cps[i] = Display(buf);
    }
}

const DisplayObj* LevelTemplate::Display(const char* name) const {
    for (const LevelDisplay& ld : kDisplayTable) {
        if (ld.id != id) continue;
        for (int32_t i = 0; i < ld.count; ++i)
            if (!std::strcmp(ld.items[i].name, name)) return &ld.items[i];
    }
    return nullptr;
}

DisplayConfig& GetDisplayConfig() {
    static DisplayConfig cfg;
    return cfg;
}

// Flash's internal sine: angle in 16.16 fixed-point degrees, 0.25-degree table (2^30 scale),
// "linear interpolation" whose weight is the low 14 bits / 65536 (a quarter of the proper weight),
// rounded to 16.16. cos(x) = sin(90 - |x|). Angles beyond 90 degrees are mirrored first.
static int32_t FlashSinFx(int64_t x) {  // x >= 0, x <= 90 * 65536
    const int64_t i = x >> 14, fr = x & 16383;
    const int64_t v = kFlashSinTab[i] + (((kFlashSinTab[i + 1] - kFlashSinTab[i]) * fr) >> 16);
    return (int32_t)((v + (1 << 13)) >> 14);
}

// The written value is converted to 16.16 fixed-point degrees by truncation and reduced into
// [-180, 180] in integers (so a written 307.77 behaves like floor, not trunc, of -52.23): exact on
// 12,216 in-game matrices and all 14,500 calibration matrices.
FlashMatrix FlashRotationMatrix(double rotDeg) {
    const int64_t D90 = 90LL * 65536, D180 = 180LL * 65536;
    int64_t xf = (int64_t)std::trunc(rotDeg * 65536);
    while (xf > D180) xf -= 2 * D180;
    while (xf < -D180) xf += 2 * D180;
    const int64_t x = xf < 0 ? -xf : xf;
    const int32_t sn = FlashSinFx(x > D90 ? D180 - x : x);
    const int32_t cs = x <= D90 ? FlashSinFx(D90 - x) : -FlashSinFx(x - D90);
    return FlashMatrix{cs, xf < 0 ? -sn : sn};
}

// PlayerBox local box is (-210,-210)-(210,210) twips (children 20/21/22). Flash's getBounds of a
// rotated object is the axis-aligned box of its transformed local box, each half-extent rounded to
// the nearest twip (calibration 3.6: exact on E1, E5 and all E6 control shapes).
Rect BallBounds(double sx, double sy, double rotDeg, int adj) {
    const double x = std::llround(sx * 20), y = std::llround(sy * 20);  // sprite x/y are whole twips
    const FlashMatrix m = FlashRotationMatrix(rotDeg);
    const int64_t a = std::llabs((int64_t)m.a) + adj, b = std::llabs((int64_t)m.b) + adj;
    const int64_t h2 = (int64_t)PLAYER_SPRITE_WIDTH_TWIPS / 2;
    const double n = (double)((h2 * (a + b) + 32768) >> 16);
    return Rect{x - n, y - n, x + n, y + n};
}

// Shipik.sign / PointInTriangle, exactly as written (strict ">" -> a point on an edge is outside).
static bool ShipikSign(double p1x, double p1y, double p2x, double p2y, double p3x, double p3y) {
    return (p1x - p3x) * (p2y - p3y) - (p2x - p3x) * (p1y - p3y) > 0;
}
static bool ShipikPointInTriangle(double x, double y) {
    const double w = 6, h = 9.65;  // Shipik.w, Shipik.h
    const bool d1 = ShipikSign(x, y, 0, 0, w / 2, -h);
    const bool d2 = ShipikSign(x, y, w / 2, -h, w, 0);
    const bool d3 = ShipikSign(x, y, w, 0, 0, 0);
    return d1 == d2 && d2 == d3;
}
// distance (px) from (x,y) to the triangle's boundary, for the uncertainty margin
static double ShipikEdgeDistance(double x, double y) {
    const double ax[3] = {0, 3, 6}, ay[3] = {0, -9.65, 0};
    double best = 1e300;
    for (int e = 0; e < 3; ++e) {
        const double x0 = ax[e], y0 = ay[e], x1 = ax[(e + 1) % 3], y1 = ay[(e + 1) % 3];
        const double dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
        double t = ((x - x0) * dx + (y - y0) * dy) / L2;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        best = std::fmin(best, std::hypot(x - (x0 + t * dx), y - (y0 + t * dy)));
    }
    return best;
}

// Standardized spike check, calibrated in docs/STATS_LOGGING.md 3.8 (FP 11.4, rb1_calib_mathspikes.tsv):
//  - localToGlobal truncates the point to twips, applies the 16.16 display matrix and rounds to the nearest
//    twip (E8a 16,000/16,000); globalToLocal for pure translations is exact twip subtraction;
//  - Shipik.testPoint: Level point (twips / 20) in getBounds(Level) (doubles, left/top inclusive), minus
//    cover.x/y (= dp through the DisplayObject setter, i.e. truncated to twips), L.localToGlobal truncates to
//    twips, S.globalToLocal subtracts the Shipik origin, strict sign test (E8b 26,040/26,040);
//  - whole HitTestObjectControlPoints: E8c 30,000/30,000.
// Only translated Shipiks are calibrated; rotated/scaled ones (later levels) use exact doubles and count
// decisions within SPIKE_EDGE_MARGIN twips of an edge as uncertain.
SpikeResult BallHitsSpike(double sx, double sy, double rotDeg, double dpx, double dpy, const SpikeObj& s) {
    const double bx0 = s.bx0 / 20.0, by0 = s.by0 / 20.0, bw = (s.bx1 - s.bx0) / 20.0, bh = (s.by1 - s.by0) / 20.0;
    if (sx + 11 < bx0 || sx - 11 > bx0 + bw || sy + 11 < by0 || sy - 11 > by0 + bh) return {false, false};
    const int64_t X = std::llround(sx * 20), Y = std::llround(sy * 20);  // sprite x/y: whole twips
    const FlashMatrix fm = FlashRotationMatrix(rotDeg);
    const double coverX = SpriteCoord(dpx), coverY = SpriteCoord(dpy);  // Shipik.updateCover: cover.x = dp[0]
    // Translations and exact quarter turns / mirrors (entries 0 or +-1) map whole twips to whole twips, so
    // Flash's globalToLocal has nothing to round; anything else is not calibrated yet.
    auto unit = [](double v) { return v == 0 || v == 1 || v == -1; };
    const bool plain = unit(s.a) && unit(s.b) && unit(s.c) && unit(s.d);
    const int64_t ia = (int64_t)s.a, ib = (int64_t)s.b, ic = (int64_t)s.c, id = (int64_t)s.d, idet = ia * id - ib * ic;
    const int64_t ox = std::llround(s.tx * 20), oy = std::llround(s.ty * 20);
    const double r = PLAYER_SPRITE_WIDTH_TWIPS / 20.0 / 2;  // PlayerBox: this.width / 2
    const double step = 2 * AS3_PI / 16;
    bool hit = false, uncertain = false;
    for (int k = 0; k < 16; ++k) {
        // testPoints[k] = (r cos(step k), r sin(step k)); localToGlobal: to twips (truncate), matrix, round
        const int64_t qx = (int64_t)std::trunc(r * as3_cos(step * k) * 20), qy = (int64_t)std::trunc(r * as3_sin(step * k) * 20);
        const int64_t gx = X + ((fm.a * qx - fm.b * qy + 32768) >> 16);
        const int64_t gy = Y + ((fm.b * qx + fm.a * qy + 32768) >> 16);
        const double px = gx / 20.0, py = gy / 20.0;  // p.globalToLocal(point)
        if (!(px >= bx0 && px < bx0 + bw && py >= by0 && py < by0 + bh)) continue;  // getBounds(p).containsPoint
        const double qxs = px - coverX, qys = py - coverY;                           // point.subtract(cover)
        const int64_t tx = (int64_t)std::trunc(qxs * 20), ty = (int64_t)std::trunc(qys * 20);  // p.localToGlobal
        double ux, uy;
        if (plain) {
            const int64_t vx = tx - ox, vy = ty - oy;
            ux = (double)((id * vx - ic * vy) * idet) / 20.0;  // inverse of a 0/+-1 matrix = adjugate * det
            uy = (double)((-ib * vx + ia * vy) * idet) / 20.0;
        } else {
            const double det = s.a * s.d - s.b * s.c;
            const double vx = tx / 20.0 - s.tx, vy = ty / 20.0 - s.ty;
            ux = (s.d * vx - s.c * vy) / det;
            uy = (-s.b * vx + s.a * vy) / det;
            if (ShipikEdgeDistance(ux, uy) < SPIKE_EDGE_MARGIN / 20.0) uncertain = true;
        }
        if (ShipikPointInTriangle(ux, uy)) hit = true;
    }
    return {hit, uncertain};
}

bool RectsHit(const Rect& a, const Rect& b) {
    const double ox = std::fmin(a.x1, b.x1) - std::fmax(a.x0, b.x0);
    const double oy = std::fmin(a.y1, b.y1) - std::fmax(a.y0, b.y0);
    return GetDisplayConfig().inclusive ? (ox >= 0 && oy >= 0) : (ox > 0 && oy > 0);
}

bool LevelTemplate::HasPlacement(const char* name) const {
    for (int32_t i = 0; i < placements->count; ++i)
        if (!std::strcmp(placements->items[i].name, name)) return true;
    return false;
}

const RawPlacement& LevelTemplate::Place(const char* name) const {
    for (int32_t i = 0; i < placements->count; ++i)
        if (!std::strcmp(placements->items[i].name, name)) return placements->items[i];
    std::fprintf(stderr, "level %d: no placement named '%s'\n", id, name);
    fatal("missing placement");
}

int32_t LevelTemplate::CheckpointCount() const {
    int32_t n = 0;
    while (n < 5) {  // Level(): for i < 5, stop at the first missing checkPoint<i>
        char buf[32];
        std::snprintf(buf, sizeof buf, "checkPoint%d", n);
        if (!HasPlacement(buf)) break;
        ++n;
    }
    return n;
}

static double PlacementX(const RawPlacement& p) { return p.tx / 20.0; }
static double PlacementY(const RawPlacement& p) { return p.ty / 20.0; }
// DisplayObject.rotation of a timeline-placed clip. Unrotated (b == c == 0, a > 0): 0 whatever the
// scale. Rotated: Flash does NOT return atan2 of the stored 16.16 matrix for timeline clips (Level 8
// killSpusk2: matrix -65400/-566 gives atan2 = -179.50414982270968, Flash reports -179.50430297851562,
// ~10 float32 ulps away, an internal approximation). Until that routine is known, the values Flash
// reports (docs/STATS_LOGGING.md 3.9, E9a dump per level) are used verbatim.
struct TimelineRotation { int32_t level; const char* name; uint64_t bits; bool measured; };
static const TimelineRotation kTimelineRotations[] = {
    {8, "killSpusk2", 0xc066702340000000ULL, true},  // rb1_calib_l8.tsv E9a: -179.50430297851562
    // Level 4 axes: recovered from the tick-0 body angle in rb1_stats_deathwarp*.tsv (angle = rotation * PI/180
    // has exactly one double solution each); float32-precision values like killSpusk2's.
    {4, "axe1", 0xc047d49680000000ULL, true},  // -47.66084289550781 (atan2 of the matrix: -47.660612218870874)
    {4, "axe2", 0x4047ae8d00000000ULL, true},  // 47.363677978515625 (atan2 of the matrix: 47.36356874825479)
};
bool LookupTimelineRotation(int32_t level, const char* name, double& out, bool* measured) {
    for (const TimelineRotation& t : kTimelineRotations)
        if (t.level == level && !std::strcmp(t.name, name)) {
            std::memcpy(&out, &t.bits, 8);
            if (measured) *measured = t.measured;
            return true;
        }
    return false;
}
int32_t g_provisionalRotations = 0;
static double PlacementRotation(int32_t level, const RawPlacement& p) {
    if (p.b == 0 && p.c == 0 && p.a > 0) return 0;
    double v;
    bool measured = false;
    if (LookupTimelineRotation(level, p.name, v, &measured)) {
        if (!measured) ++g_provisionalRotations;
        return v;
    }
    fatal("rotated timeline placement without a measured rotation (log E9a for this level, docs/STATS_LOGGING.md 3.9)");
}

// ---------------------------------------------------------------- body helpers

int32_t Sim::Geom(const std::string& key, const ShapeDef& def) {
    auto it = tpl->geomCache.find(key);
    if (it != tpl->geomCache.end()) return it->second;
    if (tpl->frozen) fatal("new geometry requested after template freeze");
    int32_t g = tpl->geoms.Add(def);
    tpl->geomCache[key] = g;
    return g;
}

int32_t Sim::BeginBody(const char* name) {
    const RawPlacement& p = tpl->Place(name);
    double x = PlacementX(p), y = PlacementY(p);
    double rot = createAtSprite >= 0 ? spriteRot[createAtSprite] : PlacementRotation(tpl->id, p);
    double rotW = createAtSprite >= 0 ? spriteRotW[createAtSprite] : rot;
    if (createAtSprite >= 0) {  // CreateBody reads the sprite's CURRENT x/y/rotation (getter values)
        x = spriteX[createAtSprite];
        y = spriteY[createAtSprite];
    }
    BodyDef bd;
    bd.position = Vec2(x / PHYS_SCALE, y / PHYS_SCALE);
    bd.angle = rot * (AS3_PI / 180);
    int32_t idx = -1;
    for (size_t i = 0; i < tpl->bodyNames.size(); ++i)
        if (tpl->bodyNames[i] == name) idx = (int32_t)i;
    if (idx < 0) {
        if (tpl->frozen) fatal("new body name after template freeze");
        tpl->bodyNames.push_back(name);
        idx = (int32_t)tpl->bodyNames.size() - 1;
    }
    bd.userTag = 100 + idx;
    int32_t b = world.CreateBody(bd);
    hasSprite[b] = true;
    spriteX[b] = x;
    spriteY[b] = y;
    spriteRot[b] = rot;
    spriteRotW[b] = rotW;
    return b;
}

// Level.CreateBody(name, "Polygon"|"BluePolygon", density, friction, restitution, polys)
int32_t Sim::CreateBody(const char* name, const char* kind, double density, double friction, double restitution,
                        const PolyList& polys) {
    if (std::strcmp(kind, "Polygon") && std::strcmp(kind, "BluePolygon")) fatal("CreateBody: polygon kinds only");
    int32_t b = BeginBody(name);
    for (size_t i = 0; i < polys.size(); ++i) {
        ShapeDef d;
        d.type = e_polygonShape;
        d.vertexCount = (int32_t)polys[i].size();
        for (int32_t k = 0; k < d.vertexCount; ++k)
            d.vertices[k].Set(polys[i][(size_t)k].first / PHYS_SCALE, polys[i][(size_t)k].second / PHYS_SCALE);
        d.density = density;
        d.friction = friction;
        d.restitution = restitution;
        world.CreateShape(b, Geom(std::string(name) + "#" + std::to_string(i), d), d);
    }
    world.SetMassFromShapes(b);
    return b;
}

// Level.CreateBody(name, "Circle", density, friction, restitution, [size])
int32_t Sim::CreateCircleBody(const char* name, double density, double friction, double restitution, double size) {
    int32_t b = BeginBody(name);
    ShapeDef d;
    d.type = e_circleShape;
    d.radius = size / PHYS_SCALE / 2;
    d.localPosition.Set(size / PHYS_SCALE / 2, size / PHYS_SCALE / 2);
    d.density = density;
    d.friction = friction;
    d.restitution = restitution;
    world.CreateShape(b, Geom(std::string(name) + "#c", d), d);
    world.SetMassFromShapes(b);
    return b;
}

void Sim::PlayerDie() {
    // Level.PlayerDie spawns 8 debris bodies with Math.random() and destroys the player body; the level keeps
    // updating (Sim::DeadUpdate models what matters: camera + the death-warp goal/checkpoint tests).
    if (playerAlive) deathFrame = frameCount + 1;  // this Update's frame
    playerAlive = false;
}

// ---------------------------------------------------------------- level scripts

static const PolyList Box(double w, double h) { return {{{w, 0}, {w, h}, {0, h}, {0, 0}}}; }

// Level_1.as
static void L1_Construct(Sim& s) {
    s.CreateBody("startPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(200, 20));
    s.CreateBody("exitPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(300, 20));
    s.CreateBody("barier", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, {{{0, 0}, {20, 0}, {20, 50}, {0, 50}}});
}
static void L1_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 550 && s.playerAlive) s.PlayerDie();
}

// Level_2.as: pendulum (distance joint) + moving platform (prismatic joint)
enum { L2_MOVE_PLATFORM = 0 };        // lvBody
enum { L2_MOVE_DIRECTION = 0 };       // lvInt
static void L2_Construct(Sim& s) {
    World& w = s.world;
    s.CreateBody("firstPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(166, 20));
    s.CreateBody("platformTriangle", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(166, 20));
    s.CreateBody("triangleBarier", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION,
                 {{{68, 67}, {0, 67}, {68, 15}}});
    s.CreateBody("ballPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(166, 20));
    int32_t kickBall = s.CreateCircleBody("kickBall", 3 * DEFAULT_DENSITY, DEFAULT_FRICTION, DEFAULT_RESTITUTION, 54);
    s.CreateBody("jump1", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(100, 20));
    int32_t movePlatform = s.CreateBody("movePlatform", "Polygon", DEFAULT_DENSITY, 3 * DEFAULT_FRICTION,
                                        DEFAULT_RESTITUTION, {{{40, -5}, {40, 5}, {-40, 5}, {-40, -5}}});
    s.CreateBody("exitPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(166, 20));
    s.lvBody[L2_MOVE_PLATFORM] = movePlatform;

    JointDef dj;  // b2DistanceJointDef.Initialize(kickBall, m_ground, a1, a2)
    w.InitDistanceJointDef(dj, kickBall, w.groundBody, Vec2(148 / PHYS_SCALE, 305 / PHYS_SCALE),
                           Vec2(87 / PHYS_SCALE, 209 / PHYS_SCALE));
    w.CreateJoint(dj);
    JointDef pj;  // b2PrismaticJointDef.Initialize(movePlatform, m_ground, worldCenter, (1,0))
    w.InitPrismaticJointDef(pj, movePlatform, w.groundBody, w.bodies[movePlatform].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    s.lvInt[L2_MOVE_DIRECTION] = 1;
    w.CreateJoint(pj);
}
static void L2_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 550 && s.playerAlive) s.PlayerDie();
    int32_t mp = s.lvBody[L2_MOVE_PLATFORM];
    if (s.spriteX[mp] < 390) s.lvInt[L2_MOVE_DIRECTION] = 1;
    if (s.spriteX[mp] > 550) s.lvInt[L2_MOVE_DIRECTION] = -1;
    s.world.SetLinearVelocity(mp, Vec2(2 * s.lvInt[L2_MOVE_DIRECTION], 0));
}

// Level_3.as: two vertical moving platforms (prismatic joints), three spike rows (display layer)
enum { L3_MOVE1 = 0, L3_MOVE2 = 1 };  // lvBody / lvInt (direction)
static void L3_Construct(Sim& s) {
    World& w = s.world;
    s.CreateBody("firstPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(150, 20));
    s.CreateBody("shipPlatform1", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(250, 20));
    s.CreateBody("shipPlatform2", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(250, 20));
    const int32_t m1 = s.CreateBody("movePlatform1", "Polygon", DEFAULT_DENSITY, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(150, 10));
    const int32_t m2 = s.CreateBody("movePlatform2", "Polygon", DEFAULT_DENSITY, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(150, 10));
    s.CreateBody("exitPlatform", "Polygon", 0, DEFAULT_FRICTION, DEFAULT_RESTITUTION, Box(450, 20));
    s.lvBody[L3_MOVE1] = m1;
    s.lvBody[L3_MOVE2] = m2;
    // one b2PrismaticJointDef, Initialize'd twice (enableLimit/enableMotor set before the first CreateJoint)
    JointDef pj;
    w.InitPrismaticJointDef(pj, m1, w.groundBody, w.bodies[m1].sweep.c, Vec2(0, 1));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L3_MOVE1] = 1;
    w.InitPrismaticJointDef(pj, m2, w.groundBody, w.bodies[m2].sweep.c, Vec2(0, 1));
    w.CreateJoint(pj);
    s.lvInt[L3_MOVE2] = 1;
}
static void L3_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 650 && s.playerAlive) s.PlayerDie();
    const int32_t m1 = s.lvBody[L3_MOVE1], m2 = s.lvBody[L3_MOVE2];
    if (s.spriteY[m1] < 270) s.lvInt[L3_MOVE1] = 1;
    if (s.spriteY[m1] > 425) s.lvInt[L3_MOVE1] = -1;
    s.world.SetLinearVelocity(m1, Vec2(0, 3 * s.lvInt[L3_MOVE1]));
    if (s.spriteY[m2] < 116) s.lvInt[L3_MOVE2] = 1;
    if (s.spriteY[m2] > 271) s.lvInt[L3_MOVE2] = -1;
    s.world.SetLinearVelocity(m2, Vec2(0, 3 * s.lvInt[L3_MOVE2]));
}

// Level_8.as: car on two wheels (revolute joints), three crushers on prismatic joints, kill ramps,
// 11 spike rows (6 rotated 90 degrees).
enum { L8_KILLER1 = 0, L8_KILLER2 = 1, L8_KILLER3 = 2, L8_KILLSPUSK = 3, L8_KILLSPUSK2 = 4, L8_CAR = 5 };
static void L8_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstRampa", "Polygon", 0, F, R,
                 {{{217, 189}, {247, 270}, {84, 4}, {102, 11}, {115, 24}, {127, 37}, {145, 60}, {174, 100}, {197, 143}},
                  {{247, 270}, {288, 338.95}, {0, 480.95}, {0, 0}, {68, 0}, {84, 4}},
                  {{288, 338.95}, {315.95, 365.95}, {0, 480.95}},
                  {{315.95, 365.95}, {348.95, 380.95}, {0, 480.95}},
                  {{348.95, 380.95}, {390.95, 386.95}, {478.95, 480.95}, {0, 480.95}},
                  {{478.95, 354.95}, {478.95, 480.95}, {428.95, 378.95}},
                  {{428.95, 378.95}, {478.95, 480.95}, {390.95, 386.95}}});
    s.CreateBody("pol", "Polygon", 0, F, R,
                 {{{683, 0}, {683, 34.05}, {500, 34.05}, {0, 0}}, {{0, 200.05}, {0, 0}, {500, 34.05}, {500, 200.05}}});
    s.CreateBody("potolok1", "Polygon", 0, F, R, {{{198, 0}, {198, 200.05}, {0, 200.05}, {0, 0}}});
    s.CreateBody("potolok2", "Polygon", 0, F, R, {{{0, 0}, {45, 0}, {45, 200}, {0, 200}}});
    s.CreateBody("potolok3", "Polygon", 0, F, R, {{{0, 0}, {45, 0}, {45, 200}, {0, 200}}});
    s.CreateBody("potolok4", "Polygon", 0, F, R, {{{0, 0}, {125, 0}, {125, 200}, {0, 200}}});
    s.lvBody[L8_KILLSPUSK] = s.CreateBody(
        "killSpusk", "Polygon", 0, F, R,
        {{{954.95, 494.95}, {963.95, 524.95}, {874.95, 514.95}},
         {{874.95, 514.95}, {963.95, 524.95}, {882.95, 547.95}, {792.95, 547.95}, {798.95, 514.95}},
         {{182.95, 26}, {544.95, 393.95}, {522.95, 419.95}, {164, 53.95}, {115.95, 0}},
         {{0, 0}, {115.95, 0}, {111, 33.95}, {0, 33.95}},
         {{111, 33.95}, {115.95, 0}, {164, 53.95}},
         {{544.95, 393.95}, {646.95, 458.95}, {522.95, 419.95}},
         {{646.95, 458.95}, {798.95, 514.95}, {792.95, 547.95}, {632.95, 491}, {522.95, 419.95}}});
    const int32_t car = s.CreateBody(
        "car", "Polygon", D, F, R,
        {{{98, 40.75}, {91.5, 41.75}, {91.5, 39.25}, {98, 24.25}},
         {{62, 0}, {68, 16.5}, {65.75, 17}, {59.75, 0}},
         {{68, 16.5}, {98, 24.25}, {65.75, 26}, {65.75, 17}},
         {{17.25, 0.5}, {20.5, 21.75}, {13.5, 28.5}, {9.5, 31.5}, {0, 0.5}},
         {{0, 41.5}, {0, 0.5}, {6.5, 36.25}, {5.5, 41.5}},
         {{6.5, 36.25}, {0, 0.5}, {9.5, 31.5}},
         {{13.5, 28.5}, {20.5, 21.75}, {17.25, 27.5}},
         {{20.5, 21.75}, {33.25, 26}, {17.25, 27.5}},
         {{33.25, 26}, {65.75, 26}, {21.25, 27.5}, {17.25, 27.5}},
         {{21.25, 27.5}, {65.75, 26}, {25.5, 28.5}},
         {{65.75, 26}, {98, 24.25}, {28.5, 30.25}, {25.5, 28.5}},
         {{28.5, 30.25}, {98, 24.25}, {75.25, 27.5}, {30.75, 32.75}},
         {{33.75, 41.5}, {33.5, 39.25}, {63.5, 41.5}},
         {{33.5, 39.25}, {32.25, 35.5}, {63.5, 41.5}},
         {{32.25, 35.5}, {30.75, 32.75}, {64.5, 36.25}, {63.5, 41.5}},
         {{64.5, 36.25}, {30.75, 32.75}, {67.5, 31.5}},
         {{67.5, 31.5}, {30.75, 32.75}, {71.5, 28.5}},
         {{71.5, 28.5}, {30.75, 32.75}, {75.25, 27.5}},
         {{75.25, 27.5}, {98, 24.25}, {79.25, 27.5}},
         {{79.25, 27.5}, {98, 24.25}, {83.5, 28.5}},
         {{83.5, 28.5}, {98, 24.25}, {86.5, 30.25}},
         {{86.5, 30.25}, {98, 24.25}, {88.75, 32.75}},
         {{88.75, 32.75}, {98, 24.25}, {90.25, 35.5}},
         {{90.25, 35.5}, {98, 24.25}, {91.5, 39.25}}});
    s.lvBody[L8_CAR] = car;
    s.lvBody[L8_KILLSPUSK2] = s.CreateBody(
        "killSpusk2", "Polygon", 0, F, R,
        {{{290, 76}, {631.05, 409.05}, {275, 105.95}, {199, 34}},
         {{0, 0}, {199, 34}, {191, 65.95}, {-6, 34.95}},
         {{191, 65.95}, {199, 34}, {275, 105.95}},
         {{275, 105.95}, {631.05, 409.05}, {618, 439.95}},
         {{631.05, 409.05}, {717.05, 457.05}, {618, 439.95}},
         {{717.05, 457.05}, {891.05, 494.05}, {882.95, 523.95}, {705.95, 485.95}, {618, 439.95}},
         {{891.05, 494.05}, {965.05, 494.05}, {964.95, 523.95}, {882.95, 523.95}},
         {{964.95, 523.95}, {965.05, 494.05}, {1049.05, 468.05}, {1057.95, 493.95}}});
    const int32_t wheel1 = s.CreateCircleBody("koleco1", D, F, R, 23.3);
    const int32_t wheel2 = s.CreateCircleBody("koleco2", D, F, R, 23.3);
    const PolyList killer = {{{8.9, 0}, {34.9, 0}, {34.9, 200}, {22.2, 218.5}, {8.9, 200}}};
    s.lvBody[L8_KILLER1] = s.CreateBody("killer1", "Polygon", 10 * D, F, R, killer);
    s.lvBody[L8_KILLER2] = s.CreateBody("killer2", "Polygon", 10 * D, F, R, killer);
    s.lvBody[L8_KILLER3] = s.CreateBody("killer3", "Polygon", 10 * D, F, R, killer);
    s.CreateBody("finishPlatform", "Polygon", 0, F, R,
                 {{{482.85, -107}, {500.05, -107}, {500.05, 20}, {482.85, 0}}, {{0, 0}, {482.85, 0}, {500.05, 20}, {0, 20}}});
    s.CreateBody("barier", "Polygon", 0, F, R, {{{24.75, 0}, {34.5, 33.75}, {0, 33.75}, {10, 0}}});
    // one b2PrismaticJointDef Initialize'd three times (defaults: no limit, no motor)
    JointDef pj;
    for (int k = L8_KILLER1; k <= L8_KILLER3; ++k) {
        const int32_t kb = s.lvBody[k];
        w.InitPrismaticJointDef(pj, kb, w.groundBody, w.bodies[kb].sweep.c, Vec2(0, 1));
        w.CreateJoint(pj);
        s.lvInt[k] = 0;  // killerNUp = false
    }
    // one b2RevoluteJointDef: car <-> wheel at ((koleco.x + 11.65) / 30, (koleco.y + 11.65) / 30)
    JointDef rj;
    const double k1x = s.tpl->Place("koleco1").tx / 20.0, k1y = s.tpl->Place("koleco1").ty / 20.0;
    const double k2x = s.tpl->Place("koleco2").tx / 20.0, k2y = s.tpl->Place("koleco2").ty / 20.0;
    w.InitRevoluteJointDef(rj, car, wheel1, Vec2((k1x + 11.65) / PHYS_SCALE, (k1y + 11.65) / PHYS_SCALE));
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, car, wheel2, Vec2((k2x + 11.65) / PHYS_SCALE, (k2y + 11.65) / PHYS_SCALE));
    w.CreateJoint(rj);
}
static bool PlayerTouches(const Sim& s, int32_t body) {
    const PlayerContactListener& L = s.world.listener;
    for (int32_t i = 0; i < L.count; ++i)
        if (L.bodies[i] == body) return true;
    return false;
}
static void L8_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 1800 || PlayerTouches(s, s.lvBody[L8_KILLER1]) || PlayerTouches(s, s.lvBody[L8_KILLER2]) ||
        PlayerTouches(s, s.lvBody[L8_KILLER3]) || PlayerTouches(s, s.lvBody[L8_KILLSPUSK]) ||
        PlayerTouches(s, s.lvBody[L8_KILLSPUSK2])) {
        if (s.playerAlive) s.PlayerDie();
    }
    for (int k = L8_KILLER1; k <= L8_KILLER3; ++k) {
        const int32_t kb = s.lvBody[k];
        if (s.spriteY[kb] > 70) s.lvInt[k] = 1;
        if (s.spriteY[kb] < -4) s.lvInt[k] = 0;
        if (s.lvInt[k]) s.world.SetLinearVelocity(kb, Vec2(0, -4));
    }
}

// Level_4.as: three crushers (prismatic, pushed up at -2), two swinging axes (revolute, timeline-rotated),
// a drop platform that is destroyed and re-created as a dynamic body when the ball touches it.
enum { L4_KB1 = 0, L4_KB2 = 1, L4_KB3 = 2, L4_AXE1 = 3, L4_AXE2 = 4, L4_DROP = 5 };
static void L4_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("mainPlatform", "Polygon", 0, F, R, {{{810, 0}, {810, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("firstFloor", "Polygon", 0, F, R, {{{200, 0}, {200, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("secondFloor1", "Polygon", 0, F, R, {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("secondFloor2", "Polygon", 0, F, R, {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("secondFloor3", "Polygon", 0, F, R, {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("step", "Polygon", 0, F, R,
                 {{{117, -10}, {202, -10}, {117, 12}}, {{92, 12}, {117, 12}, {92, 34}}, {{117, 12}, {202, -10}, {150, 16}, {92, 34}},
                  {{70, 34}, {92, 34}, {150, 122}, {70, 57}}, {{45, 57}, {70, 57}, {45, 76}}, {{24, 76}, {45, 76}, {24, 99}},
                  {{45, 76}, {70, 57}, {24, 99}}, {{0, 99}, {24, 99}, {0, 122}}, {{24, 99}, {70, 57}, {150, 122}, {0, 122}},
                  {{150, 122}, {92, 34}, {150, 16}}, {{202, 16}, {150, 16}, {202, -10}}});
    s.lvBody[L4_DROP] = s.CreateBody("dropPlatform", "Polygon", 0, F, R, {{{130, 0}, {130, 10}, {0, 10}, {0, 0}}});
    s.CreateBody("axePlatform", "Polygon", 0, F, R, {{{850, 0}, {850, 20}, {0, 20}, {0, 0}}});
    const PolyList boom = {{{19.5, -267.5}, {19.5, -31.5}, {-19.5, -31.5}, {-19.5, -267.5}},
                           {{-19.5, -31.5}, {19.5, -31.5}, {50.5, 0.5}, {-47.5, 0.5}}};
    s.lvBody[L4_KB1] = s.CreateBody("killBoom1", "Polygon", D, F, R, boom);
    s.lvBody[L4_KB2] = s.CreateBody("killBoom2", "Polygon", D, F, R, boom);
    s.lvBody[L4_KB3] = s.CreateBody("killBoom3", "Polygon", D, F, R, boom);
    const PolyList axe = {{{25.5, -1.5}, {40.5, -1.5}, {40.5, 113.5}, {25.5, 113.5}},
                          {{0.5, 116.5}, {25.5, 113.5}, {40.5, 113.5}, {67.5, 116.5}, {67.5, 175.5}, {0.5, 175.5}}};
    s.lvBody[L4_AXE1] = s.CreateBody("axe1", "Polygon", 2 * D, F, R, axe);
    s.lvBody[L4_AXE2] = s.CreateBody("axe2", "Polygon", 2 * D, F, R, axe);
    JointDef pj;
    for (int k = L4_KB1; k <= L4_KB3; ++k) {
        const int32_t kb = s.lvBody[k];
        w.InitPrismaticJointDef(pj, kb, w.groundBody, w.bodies[kb].sweep.c, Vec2(0, 1));
        pj.enableLimit = false;
        pj.enableMotor = false;
        w.CreateJoint(pj);
    }
    // AS3 sets killBoom1Up = true three times: killBoom2Up / killBoom3Up start false
    s.lvInt[L4_KB1] = 1;
    s.lvInt[L4_KB2] = 0;
    s.lvInt[L4_KB3] = 0;
    JointDef rj;
    w.InitRevoluteJointDef(rj, s.lvBody[L4_AXE1], w.groundBody, Vec2(785 / PHYS_SCALE, 127 / PHYS_SCALE));
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, s.lvBody[L4_AXE2], w.groundBody, Vec2(1077 / PHYS_SCALE, 127 / PHYS_SCALE));
    w.CreateJoint(rj);
}
static void L4_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 650 || PlayerTouches(s, s.lvBody[L4_KB1]) || PlayerTouches(s, s.lvBody[L4_KB2]) ||
        PlayerTouches(s, s.lvBody[L4_KB3]) || PlayerTouches(s, s.lvBody[L4_AXE1]) || PlayerTouches(s, s.lvBody[L4_AXE2])) {
        if (s.playerAlive) s.PlayerDie();
    }
    if (PlayerTouches(s, s.lvBody[L4_DROP])) {
        // DestroyBody + CreateBody("dropPlatform", 3 * density): the clip is reused at its current state
        const int32_t old = s.lvBody[L4_DROP];
        s.world.DestroyBody(old);
        s.createAtSprite = old;
        s.lvBody[L4_DROP] = s.CreateBody("dropPlatform", "Polygon", 3 * DEFAULT_DENSITY, DEFAULT_FRICTION, DEFAULT_RESTITUTION,
                                         {{{130, 0}, {130, 10}, {0, 10}, {0, 0}}});
        s.createAtSprite = -1;
        s.hasSprite[old] = false;
    }
    for (int k = L4_KB1; k <= L4_KB3; ++k) {
        const int32_t kb = s.lvBody[k];
        if (s.spriteY[kb] > 436) s.lvInt[k] = 1;
        if (s.spriteY[kb] < 327) s.lvInt[k] = 0;
        if (s.lvInt[k]) s.world.SetLinearVelocity(kb, Vec2(0, -2));
    }
}

static void NotImplemented(Sim&) { fatal("level not implemented yet"); }

const LevelScript& GetLevelScript(int32_t id) {
    static const LevelScript scripts[] = {
        {1, L1_Construct, L1_Update, true},
        {2, L2_Construct, L2_Update, true},
        {3, L3_Construct, L3_Update, true},
        {4, L4_Construct, L4_Update, true},
        {8, L8_Construct, L8_Update, true},
    };
    for (const LevelScript& ls : scripts)
        if (ls.id == id) return ls;
    static LevelScript missing{0, NotImplemented, NotImplemented, false};
    missing.id = id;
    return missing;
}

// ---------------------------------------------------------------- construction

void Sim::Load(LevelTemplate* t, int32_t checkpoint) {
    tpl = t;
    lastCheckNum = checkpoint;
    frameCount = 0;
    displayUncertain = 0;
    Restart();
    tpl->frozen = true;  // geometry/body names are now fixed; later constructions only look up
}

void Sim::Restart() {
    const LevelScript& script = GetLevelScript(tpl->id);
    if (!script.implemented) fatal("level script not implemented yet");
    for (int32_t i = 0; i < CAP_BODIES; ++i) {
        hasSprite[i] = false;
        spriteX[i] = spriteY[i] = spriteRot[i] = spriteRotW[i] = 0;
    }
    for (int32_t i = 0; i < LV_VARS; ++i) lvBody[i] = lvInt[i] = 0;

    // --- Level() constructor
    AABB worldAABB;
    worldAABB.lowerBound.Set(-1000, -1000);
    worldAABB.upperBound.Set(1000, 1000);
    world.Init(&tpl->geoms, worldAABB, Vec2(0, 10), true);
    world.listener.enabled = true;  // m_world.SetContactListener(myContactListener)

    int32_t nCheck = tpl->CheckpointCount();
    if (lastCheckNum >= nCheck) fatal("checkpoint index out of range");
    // Level(): levelAim.stop(); each checkPoint<i>.stop(), gotoAndStop(5) if already collected
    aimFrame = 1;
    for (int32_t i = 0; i < 5; ++i) cpFrame[i] = (i < nCheck && lastCheckNum > i) ? 5 : 1;
    char cpName[32];
    std::snprintf(cpName, sizeof cpName, "checkPoint%d", lastCheckNum);
    const RawPlacement& cp = tpl->Place(cpName);
    double px = PlacementX(cp), py = PlacementY(cp);

    // PlayerBox(x, y, world, sprite)
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
    hasSprite[playerBody] = true;
    spriteX[playerBody] = SpriteCoord(px);
    spriteY[playerBody] = SpriteCoord(py);
    spriteRot[playerBody] = spriteRotW[playerBody] = 0;
    // PlayerBox(): m_spriteIn.x = -this.x + Game.stageWidth / 2 (camera centred on the ball)
    camX = SpriteCoord(-spriteX[playerBody] + 550.0 / 2);
    camY = SpriteCoord(-spriteY[playerBody] + 400.0 / 2);
    camTween = false;
    dpX = dpY = 0;
    deadTicks = 0;
    deathFrame = winFrame = -1;

    // --- Level_N() constructor
    script.construct(*this);

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

// Tweener.onEnterFrame (driven by COMM "TweenEvent"): the camera tween added by the previous Update is
// evaluated at t = 1 of d = 31 frames: easeOutExpo = c * 1.001 * (-2^(-10 t / d) + 1) + b, then the
// DisplayObject setter truncates to twips.
void Sim::CameraStep() {
    if (!camTween) return;
    static const double p = std::pow(2.0, -10 * 1.0 / 31);  // Math.pow(2, -10 * t / d), t = 1
    const double bx = camX, cx = camTargetX - bx;
    const double by = camY, cy = camTargetY - by;
    camX = SpriteCoord(cx * 1.001 * (-p + 1) + bx);
    camY = SpriteCoord(cy * 1.001 * (-p + 1) + by);
}

void Sim::LevelUpdate(bool left, bool up, bool right) {
    // dp[0] = x; broadcast("TweenEvent") unless isGless; dp[0] -= x
    {
        const double ox = camX, oy = camY;
        if (!gless) CameraStep();
        dpX = ox - camX;
        dpY = oy - camY;
    }
    world.Step(LEVEL_TIMESTEP, LEVEL_ITERATIONS);

    // sprite sync: every non-static body whose userData is a Sprite
    for (int32_t b = world.bodyList; b != -1; b = world.bodies[b].next) {
        const Body& bb = world.bodies[b];
        if (hasSprite[b] && !bb.IsStatic()) {
            spriteX[b] = SpriteCoord(bb.xf.position.x * PHYS_SCALE);
            spriteY[b] = SpriteCoord(bb.xf.position.y * PHYS_SCALE);
            spriteRot[b] = SpriteRotation(bb.sweep.a);
            spriteRotW[b] = SpriteRotationWritten(bb.sweep.a);
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
    // Tweener.addTween(m_sprite, {x: scaleX * (-playerBox.x + stageWidth / 2), y: ..., useFrames, time 31,
    // easeOutExpo}); overwrites the previous camera tween. scaleX/scaleY stay 1 (scaleTimer never runs).
    camTargetX = 1.0 * (-spriteX[playerBody] + 550.0 / 2);
    camTargetY = 1.0 * (-spriteY[playerBody] + 400.0 / 2);
    camTween = true;

    DisplayUpdate();
    if (gless) CameraStep();  // broadcast("TweenEvent") at the end of Level.Update

    GetLevelScript(tpl->id).update(*this);  // Level_N.Update after super.Update
}

void Sim::PlayerWin() {
    if (!isTimeStop) winFrame = frameCount + 1;  // this Update's frame (frameCount is bumped after Update)
    isTimeStop = true;  // Game.tPause = 0: the game loop stops calling Update
    aimFrame = 2;       // levelAim.play()
    Body& b = world.bodies[playerBody];
    b.linearDamping = 3;
    b.angularDamping = 3;
}

// Level.Update: levelAim win test, spikes (TODO), checkpoints.
// hitTestObject(playerBox, target) compares global (stage) boxes. Game sits at the stage origin with
// scale 1 (E8a: global - Level-local == Level.x/y), and the Level's own offset is the camera.
//  - alive: both are children of the Level, the camera cancels -> compare Level-local boxes;
//  - dead: PlayerBox.Kill() removed the ball from the display list, so its "global" box is its own
//    transform (frozen Level-local x/y/rotation), while the target is still shifted by the camera.
//    Level.Update has no IsLive() guard on the goal or checkpoint tests: this is the death warp.
bool Sim::BallHitsTarget(const DisplayObj& o) {
    const double sx = spriteX[playerBody], sy = spriteY[playerBody], rw = spriteRotW[playerBody];
    const double ox = playerAlive ? 0 : camX * 20, oy = playerAlive ? 0 : camY * 20;
    const Rect r{o.x0 + ox, o.y0 + oy, o.x1 + ox, o.y1 + oy};
    const bool lo = RectsHit(BallBounds(sx, sy, rw, -1), r), hi = RectsHit(BallBounds(sx, sy, rw, +1), r);
    if (lo != hi) ++displayUncertain;
    return RectsHit(BallBounds(sx, sy, rw, 0), r);
}

// Level.Update after the camera tween: levelAim test, spikes, checkpoints (in this order).
void Sim::DisplayUpdate() {
    if (tpl->aim && aimFrame == 1 && BallHitsTarget(*tpl->aim)) PlayerWin();  // no IsLive() guard
    // Spikes: any Shipik/Ships10 hit by a control point kills a live ball (PlayerDie is guarded here).
    if (playerAlive) {
        for (int32_t i = 0; i < tpl->spikeCount; ++i) {
            const SpikeResult r = BallHitsSpike(spriteX[playerBody], spriteY[playerBody], spriteRotW[playerBody], dpX, dpY, tpl->spikes[i]);
            if (r.uncertain) ++displayUncertain;
            if (r.hit) {
                PlayerDie();
                break;
            }
        }
    }
    // Checkpoints: no IsLive() guard. After a spike death in this same Update the ball is already off
    // the display list, so the dead-ball rule applies from here on.
    for (int32_t i = 0; i < 5; ++i) {
        const DisplayObj* o = tpl->cps[i];
        if (!o) continue;
        if (cpFrame[i] == 1 && BallHitsTarget(*o)) {
            if (i > lastCheckNum) lastCheckNum = i;
            cpFrame[i] = 2;  // checkPoint<i>.play()
        }
    }
}

// Level.Update while the ball is dead (until R restarts the level). The world keeps stepping in Flash,
// but only with the random debris and the level's machinery; nothing of it survives a restart, and the
// ball's sprite is frozen (its body left the world). What matters is simulated: the camera tween toward
// the frozen ball, and the goal/checkpoint tests with the dead-ball rule.
void Sim::DeadUpdate() {
    const double ox = camX, oy = camY;
    if (!gless) CameraStep();
    dpX = ox - camX;
    dpY = oy - camY;
    camTargetX = 1.0 * (-spriteX[playerBody] + 550.0 / 2);
    camTargetY = 1.0 * (-spriteY[playerBody] + 400.0 / 2);
    camTween = true;
    DisplayUpdate();
    if (gless) CameraStep();
    ++deadTicks;
}

void Sim::Tick(uint8_t input) {
    if (isTimeStop) return;  // Game.tPause == 0 after PlayerWin: no more Level.Update calls
    // Game.UpdateHandler (playback): Left = v>=4, Up = v>=6||v==2||v==3, Right = v%2==1
    bool left = input >= 4;
    bool up = input >= 6 || input == 2 || input == 3;
    bool right = input % 2 == 1;
    if (playerAlive) LevelUpdate(left, up, right);
    else DeadUpdate();
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
    s.sx = spriteX[playerBody];
    s.sy = spriteY[playerBody];
    s.sr = spriteRot[playerBody];
    s.timeStop = isTimeStop;
    return s;
}

std::string Sim::BodyName(int32_t body) const {
    if (body == world.groundBody) return "ground";
    int32_t tag = world.bodies[body].userTag;
    if (tag >= 100 && tag - 100 < (int32_t)tpl->bodyNames.size()) return tpl->bodyNames[(size_t)(tag - 100)];
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
