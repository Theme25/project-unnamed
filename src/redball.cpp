// redball.cpp - game-side logic (Level.as / Level_N.as / PlayerBox.as /
// Game.UpdateHandler) driving the Box2D port.
#include "redball.h"
#include "level_polys.h"
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
        const double ex = x - (x0 + t * dx), ey = y - (y0 + t * dy);
        best = std::fmin(best, std::sqrt(ex * ex + ey * ey));  // sqrt is exact everywhere; hypot is not
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
    // Levels 9-17: PROVISIONAL (measured = false) = atan2(b, a) of the stored matrix. Flash's own getter differs from
    // this by ~1e-4 degrees on timeline clips (see above), so a rotated BODY clip is only exact once its value
    // is replaced by the one from an E9a dump (static clips) or from a log's tick-0 angle (dynamic clips).
    // g_provisionalRotations counts how many of these a level construction used.
    {9, "firstCrank", 0x4050b1ad40000000ULL, true},   // rb1_calib_L9.tsv E9a: 66.776199340820312 (atan2: 66.77591509403953)
    {9, "secondCrank", 0xc039bbdb00000000ULL, true},  // rb1_calib_L9.tsv E9a: -25.733810424804688 (atan2: -25.733590917056688)
    {10, "afterJump", 0xc0667b3020000000ULL, true},  // rb1_calib_L10.tsv E9a: -179.8502197265625 (atan2: -179.8494982985548)
    {11, "kolesoTrain1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "kolesoTrain2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "train", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "triangle", 0x3fe81d4000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.753570556640625
    {11, "vagon1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon3", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon4", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon5", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon6", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon7", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon8", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon9", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon10", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "vagon11", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_1_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_1_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_2_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_2_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_3_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_3_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_4_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_4_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_5_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_5_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_6_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_6_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_7_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_7_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_8_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_8_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_9_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_9_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_10_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_10_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_11_1", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {11, "koleso_11_2", 0x3fc6450000000000ULL, true},  // rb1_calib_L11.tsv E9a: 0.173980712890625 (whole train line)
    {13, "kingStar1", 0xc02a43e200000000ULL, true},  // rb1_calib_L13.tsv E9a: -13.132583618164062
    {16, "axe1", 0xc05201d69b7c0003ULL, false},  // provisional atan2 = -72.0287235938013
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

void (*g_playerDieHook)(const Sim&) = nullptr;
void Sim::PlayerDie() {
    // Level.PlayerDie: OutControl(); 8 debris bodies (playerDiePart0-7, positions from Math.random(): NOT simulated);
    // playerBox.Kill() -> m_world.DestroyBody(ball) and the ball leaves the display list (its sprite stays frozen).
    // The level keeps running its full Update afterwards (world step, moving parts, Level_N.Update), see Sim::Tick.
    if (!playerAlive) return;
    deathFrame = frameCount + 1;  // this Update's frame
    if (g_playerDieHook) g_playerDieHook(*this);
    playerAlive = false;
    world.DestroyBody(playerBody);
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

// ---------------------------------------------------------------- shared helpers for levels 5-14
// Level.DestroyBody: only if the body is still in the world; removes its clip, then m_world.DestroyBody.
static void LevelDestroyBody(Sim& s, int32_t b) {
    if (b < 0 || !s.world.bodies[b].inWorld) return;
    s.hasSprite[b] = false;
    s.world.DestroyBody(b);
}
// m_world.DestroyBody(old) + CreateBody(name, ...) on the same clip (drop platforms of levels 4/6/14)
static int32_t RecreateBody(Sim& s, int32_t old, const char* name, double density, const PolyList& polys) {
    s.world.DestroyBody(old);
    s.createAtSprite = old;
    const int32_t b = s.CreateBody(name, "Polygon", density, DEFAULT_FRICTION, DEFAULT_RESTITUTION, polys);
    s.createAtSprite = -1;
    s.hasSprite[old] = false;
    return b;
}
// playerBox.hitTestObject(clip) for a level's switch clip (alive or dead rule)
static bool SwitchHit(Sim& s, const char* clip) {
    const DisplayObj* o = s.tpl->Display(clip);
    return o && s.BallHitsTarget(*o);
}

// Level_5.as: blue switch removes a barrier, green switch removes a platform.
enum { L5_BLUEBARIER = 0, L5_GREENPLAT = 1 };
static void L5_Construct(Sim& s) {
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstPlatform", "Polygon", 0, F, R,
                 {{{298.45, 0}, {298.45, 0.3}, {0, 20}, {0, 0}}, {{298.45, 0.3}, {300, 0.3}, {300, 20}, {0, 20}}});
    s.lvBody[L5_BLUEBARIER] = s.CreateBody("blueBarier", "Polygon", 0, F, R, {{{0, 0}, {10, 0}, {10, 100}, {0, 100}}});
    s.CreateBody("mainPlatform", "Polygon", 0, F, R, {{{594.9, 0}, {594.9, 136}, {0, 136}, {0, 116}, {195.95, 0}}});
    s.CreateBody("rightBarier", "Polygon", 0, F, R,
                 {{{0, 0}, {30, 0}, {1.05, 251}, {0, 251}}, {{1.05, 251}, {30, 0}, {30, 251.95}, {1.05, 251.95}}});
    s.CreateBody("jumpPlatform", "Polygon", D, F, R, {{{226.95, 0}, {226.95, 7}, {0, 7}, {0, 0}}});
    s.lvBody[L5_GREENPLAT] = s.CreateBody("greenPlatform", "Polygon", 0, F, R, {{{85, 0}, {85, 15}, {0, 15}, {0, 0}}});
    s.CreateCircleBody("littleBall", D, F, R, 13);
    s.CreateCircleBody("bigBall", 2 * D, F, R, 83.5);
}
static void L5_Switches(Sim& s) {
    if (s.switchFrame[0] == 1 && SwitchHit(s, "blueCheck")) {
        s.switchFrame[0] = 2;
        LevelDestroyBody(s, s.lvBody[L5_BLUEBARIER]);
    }
    if (s.switchFrame[1] == 1 && SwitchHit(s, "greenCheck")) {
        s.switchFrame[1] = 2;
        LevelDestroyBody(s, s.lvBody[L5_GREENPLAT]);
    }
}
static void L5_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 650 && s.playerAlive) s.PlayerDie();
    L5_Switches(s);
}

// Level_6.as: motorised spinner and three spinning "back balls" (revolute motors), three drop platforms.
enum { L6_DROP1 = 0, L6_DROP2 = 1, L6_DROP3 = 2 };
static const PolyList kL6Plat = {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}};
static void L6_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("rampa", "Polygon", 0, F, R,
                 {{{66.9, 0}, {66.9, 22.15}, {0, 0}}, {{66.9, 22.15}, {69.8, 47.2}, {0, 0}}, {{69.8, 47.2}, {72.9, 68.15}, {0, 0}},
                  {{72.9, 68.15}, {77.9, 90.15}, {0, 0}}, {{77.9, 90.15}, {83.9, 108.15}, {0, 0}}, {{83.9, 108.15}, {92.9, 127.15}, {0, 0}},
                  {{92.9, 127.15}, {103.9, 146.15}, {0, 0}}, {{103.9, 146.15}, {114.9, 162.15}, {0, 261.1}, {0, 0}},
                  {{114.9, 162.15}, {127.9, 174.15}, {0, 261.1}}, {{127.9, 174.15}, {146.35, 187.35}, {0, 261.1}},
                  {{146.35, 187.35}, {165.9, 194.65}, {0, 261.1}}, {{165.9, 194.65}, {183.4, 197.65}, {279.9, 261.1}, {0, 261.1}},
                  {{279.9, 138.15}, {279.9, 261.1}, {271.9, 155.65}}, {{271.9, 155.65}, {279.9, 261.1}, {261.9, 171.65}},
                  {{261.9, 171.65}, {279.9, 261.1}, {250.4, 183.65}}, {{250.4, 183.65}, {279.9, 261.1}, {234.9, 194.15}},
                  {{234.9, 194.15}, {279.9, 261.1}, {218.9, 198.15}}, {{218.9, 198.15}, {279.9, 261.1}, {200.4, 198.65}},
                  {{200.4, 198.65}, {279.9, 261.1}, {183.4, 197.65}}});
    s.CreateBody("afterJump", "Polygon", 0, F, R, {{{182, 0}, {182, 20}, {0, 20}, {0, 0}}});
    s.lvBody[L6_DROP1] = s.CreateBody("drop1", "Polygon", 0, F, R, kL6Plat);
    s.lvBody[L6_DROP2] = s.CreateBody("drop2", "Polygon", 0, F, R, kL6Plat);
    s.lvBody[L6_DROP3] = s.CreateBody("drop3", "Polygon", 0, F, R, kL6Plat);
    const int32_t spin = s.CreateBody(
        "spin", "Polygon", D, F, R,
        {{{60.55, -65.5}, {67.75, -58.6}, {26.3, -30.2}}, {{5, -89.5}, {5, -39.75}, {-5, -39.75}, {-5, -89.5}},
         {{5, -39.75}, {26.3, -30.2}, {-24.35, -31.4}, {-24.6, -31.65}, {-5, -39.75}},
         {{-24.6, -31.65}, {-24.35, -31.4}, {-31.7, -24.5}, {-67.9, -57.8}, {-61.1, -65.2}},
         {{-24.35, -31.4}, {26.3, -30.2}, {-31.45, 24.8}, {-39.8, 4.3}, {-39.65, -5.7}, {-31.7, -24.5}},
         {{-88.95, -6.1}, {-39.65, -5.7}, {-39.8, 4.3}, {-89.05, 3.9}},
         {{26.3, -30.2}, {67.75, -58.6}, {32.8, -23}, {-24.65, 31.55}, {-66.7, 60.1}, {-31.45, 24.8}},
         {{-31.7, 24.55}, {-31.45, 24.8}, {-66.7, 60.1}}, {{-59.6, 67.1}, {-66.7, 60.1}, {-24.65, 31.55}},
         {{-4.2, 39.8}, {-24.65, 31.55}, {-4.2, 39.4}}, {{-4.85, 89.4}, {-4.2, 39.8}, {5.15, 89.5}},
         {{-4.2, 39.8}, {-4.2, 39.4}, {5.8, 39.6}, {5.15, 89.5}},
         {{-4.2, 39.4}, {-24.65, 31.55}, {32.4, 23}, {32.65, 23.2}, {26.15, 30.3}, {5.8, 39.6}},
         {{25.9, 30.6}, {26.15, 30.3}, {32.65, 23.2}, {70.2, 55.6}, {63.7, 63.2}}, {{32.65, 23.2}, {32.4, 23}, {39.75, 4.45}},
         {{32.4, 23}, {-24.65, 31.55}, {32.8, -23}, {39.65, -5.55}, {39.75, 4.45}},
         {{89.05, 4}, {39.75, 4.45}, {39.65, -5.55}, {88.95, -6}}, {{33, -22.8}, {32.8, -23}, {67.75, -58.6}}});
    s.CreateBody("voronkaLeft", "Polygon", 0, 0, R,
                 {{{0, 284.45}, {105.5, 224}, {107.5, 227}, {1.5, 288}}, {{105.5, 224}, {112.5, 216.5}, {115.5, 219}, {107.5, 227}},
                  {{112.5, 216.5}, {115.5, 208}, {115.5, 219}}, {{115.5, 208}, {115.5, 68.5}, {115.5, 219}},
                  {{115.5, 219}, {115.5, 68.5}, {119.5, 67.5}, {119.5, 208}}, {{115.5, 68.5}, {88.5, 34.5}, {92.5, 32}, {119.5, 67.5}},
                  {{88.5, 34.5}, {83.5, 0}, {88, 0}, {92.5, 32}}});
    s.CreateBody("voronkaRight", "Polygon", 0, 0, R,
                 {{{0, 306.2}, {105.5, 245.75}, {107.5, 248.75}, {1.5, 309.75}}, {{105.5, 245.75}, {113.65, 240.2}, {107.5, 248.75}},
                  {{113.65, 240.2}, {120.4, 233.95}, {115.4, 243.2}, {107.5, 248.75}},
                  {{120.4, 233.95}, {124.9, 226.45}, {123.4, 235.7}, {115.4, 243.2}},
                  {{124.9, 226.45}, {128.65, 216.95}, {127.9, 227.45}, {123.4, 235.7}},
                  {{128.65, 216.95}, {130.25, 207.95}, {131.9, 218.2}, {127.9, 227.45}},
                  {{130.25, 207.95}, {130.25, 67.5}, {134.25, 68.5}, {134.25, 207.95}, {133.4, 212.95}, {131.9, 218.2}},
                  {{130.25, 67.5}, {157.25, 32}, {161.25, 34.5}, {134.25, 68.5}}, {{157.25, 32}, {161.75, 0}, {166.25, 0}, {161.25, 34.5}}});
    s.CreateBody("rampa2", "Polygon", 0, F, R,
                 {{{129.9, 0}, {129.9, 26.4}, {58.9, 26.4}, {0, 0}}, {{279.9, 191.15}, {279.9, 284.1}, {271.9, 208.65}},
                  {{271.9, 208.65}, {279.9, 284.1}, {261.9, 224.65}}, {{261.9, 224.65}, {279.9, 284.1}, {250.4, 236.65}},
                  {{250.4, 236.65}, {279.9, 284.1}, {234.9, 247.15}}, {{234.9, 247.15}, {279.9, 284.1}, {218.9, 251.15}},
                  {{279.9, 284.1}, {0, 284.1}, {200.4, 251.65}, {218.9, 251.15}}, {{200.4, 251.65}, {0, 284.1}, {183.4, 250.65}},
                  {{183.4, 250.65}, {0, 284.1}, {165.9, 247.65}}, {{165.9, 247.65}, {0, 284.1}, {146.35, 240.35}},
                  {{146.35, 240.35}, {0, 284.1}, {127.9, 227.15}}, {{127.9, 227.15}, {0, 284.1}, {114.9, 215.15}},
                  {{114.9, 215.15}, {0, 284.1}, {103.9, 199.15}}, {{103.9, 199.15}, {0, 284.1}, {92.9, 180.15}},
                  {{0, 284.1}, {0, 0}, {83.9, 161.15}, {92.9, 180.15}}, {{83.9, 161.15}, {0, 0}, {77.9, 143.15}},
                  {{77.9, 143.15}, {0, 0}, {72.9, 121.15}}, {{72.9, 121.15}, {0, 0}, {58.9, 26.4}}});
    s.CreateBody("beforeBackBalls", "Polygon", 0, F, R, {{{182, 0}, {182, 20}, {0, 20}, {0, 0}}});
    const PolyList backBall = {
        {{14.75, -16.85}, {18.5, -12.7}, {22.25, -3.1}, {22.35, 2.5}, {18.3, 13}, {14.65, 16.95}, {3.25, 22.15}, {-2.4, 22.25},
         {-13.85, 17.7}, {-17.65, 13.9}, {-22.35, 2.4}, {-22.25, -3.15}, {-17.8, -13.65}, {-13.8, -17.65}, {-2.85, -22.15}, {2.8, -22.15}},
        {{2.8, -50}, {2.8, -22.15}, {-2.85, -22.15}, {-2.85, -50}},
        {{-49.95, -3.4}, {-22.25, -3.15}, {-22.35, 2.4}, {-50, 2.2}},
        {{-2.75, 49.95}, {-2.4, 22.25}, {3.25, 22.15}, {2.85, 50}},
        {{50, 2.25}, {22.35, 2.5}, {22.25, -3.1}, {49.9, -3.35}}};
    const int32_t bb1 = s.CreateBody("backBall1", "Polygon", D, F, R, backBall);
    const int32_t bb2 = s.CreateBody("backBall2", "Polygon", D, F, R, backBall);
    const int32_t bb3 = s.CreateBody("backBall3", "Polygon", D, F, R, backBall);
    s.CreateBody("finishPlatform", "Polygon", 0, F, R, kL6Plat);
    JointDef rj;  // one b2RevoluteJointDef reused
    w.InitRevoluteJointDef(rj, spin, w.groundBody, Vec2(722 / PHYS_SCALE, 323 / PHYS_SCALE));
    rj.motorSpeed = 0.3 * -AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    for (int32_t b : {bb1, bb2, bb3}) {
        w.InitRevoluteJointDef(rj, b, w.groundBody, w.bodies[b].xf.position);
        rj.motorSpeed = 0.2 * AS3_PI;
        rj.maxMotorTorque = 5000;
        rj.enableMotor = true;
        w.CreateJoint(rj);
    }
}
static void L6_Update(Sim& s) {
    if (s.spriteY[s.playerBody] > 1060 && s.playerAlive) s.PlayerDie();
    static const char* names[3] = {"drop1", "drop2", "drop3"};
    for (int k = L6_DROP1; k <= L6_DROP3; ++k)
        if (PlayerTouches(s, s.lvBody[k])) s.lvBody[k] = RecreateBody(s, s.lvBody[k], names[k], 3 * DEFAULT_DENSITY, kL6Plat);
}

// Level_7.as: jump platforms (prismatic, limit + motor whose force is switched by contact), moving
// platforms, swinging platform (two distance joints), spinning star (distance joint, kills), red switch
// (static Level_7.redCheckLevel: removes the red wall, survives checkpoint restarts), blue switch.
enum { L7_STAR = 0, L7_JP1 = 1, L7_JP2 = 2, L7_MP1 = 3, L7_MP2 = 4, L7_REDWALL = 5, L7_BLUEPLAT = 6 };
enum { L7I_JP1JOINT = 0, L7I_JP2JOINT = 1, L7I_MP1DIR = 2, L7I_MP2DIR = 3 };
static void L7_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstPlatform", "Polygon", 0, F, R, {{{150, 0}, {150, 20}, {0, 20}, {0, 0}}});
    s.lvBody[L7_JP1] = s.CreateBody("jumpPlatform1", "Polygon", D, F, R, {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("bigPlatform", "Polygon", 0, F, R,
                 {{{228.2, -82.45}, {228.2, -59.45}, {-264.8, -47.45}, {-264.8, -82.45}},
                  {{-264.8, -47.45}, {228.2, -59.45}, {-339.8, 11.55}},
                  {{-441.75, 11.55}, {-339.8, 11.55}, {-527.75, 61.5}},
                  {{-339.8, 11.55}, {228.2, -59.45}, {254.2, -59.45}, {254.2, -36.45}, {-527.75, 82.5}, {-527.75, 61.5}},
                  {{527.75, 29}, {527.75, 82.5}, {512.1, 42.5}}, {{512.1, 42.5}, {527.75, 82.5}, {492.9, 51.2}},
                  {{492.9, 51.2}, {527.75, 82.5}, {475.55, 53.2}}, {{527.75, 82.5}, {-527.75, 82.5}, {457.5, 51.7}, {475.55, 53.2}},
                  {{457.5, 51.7}, {-527.75, 82.5}, {415.55, 41.25}}, {{415.55, 41.25}, {-527.75, 82.5}, {309.2, 8.55}},
                  {{309.2, 8.55}, {-527.75, 82.5}, {280.2, -14.45}, {309.2, -14.45}},
                  {{280.2, -14.45}, {-527.75, 82.5}, {254.2, -36.45}, {280.2, -36.45}}});
    const int32_t potolok = s.CreateBody("potolok", "Polygon", 0, F, R,
                                         {{{257, -78}, {257, -19}, {-200, 44}, {-257, -10}, {-257, -78}},
                                          {{-200, 44}, {257, -19}, {205, 33}, {-200, 78}},
                                          {{205, 78}, {-200, 78}, {205, 33}}});
    s.CreateBody("triangle", "Polygon", 0, F, R,
                 {{{-375.95, -58}, {-155.95, -36.75}, {257.1, 31.15}, {-373.9, 55}},
                  {{404, 55}, {-373.9, 55}, {257.1, 31.15}, {404, 31.2}},
                  {{257.1, 31.15}, {-155.95, -36.75}, {112.1, -53.75}},
                  {{112.1, -53.75}, {-155.95, -36.75}, {-0.4, -126.95}}});
    const PolyList mp = {{{75, -5}, {75, 5}, {-75, 5}, {-75, -5}}};
    s.lvBody[L7_MP1] = s.CreateBody("movePlatform1", "Polygon", D, F, R, mp);
    s.lvBody[L7_MP2] = s.CreateBody("movePlatform2", "Polygon", D, F, R, mp);
    s.lvBody[L7_JP2] = s.CreateBody("jumpPlatform2", "Polygon", D, F, R, {{{100, 0}, {100, 20}, {0, 20}, {0, 0}}});
    s.CreateBody("vanna", "Polygon", 0, F, R,
                 {{{415.5, -92.5}, {415.5, -59.05}, {194.55, 92.45}, {164.55, 50.45}, {234.55, -92.5}},
                  {{-156.45, -89.5}, {-63.45, 50.45}, {-74.45, 92.45}, {-415.45, -59.55}, {-415.45, -89.5}},
                  {{-63.45, 50.45}, {164.55, 50.45}, {194.55, 92.45}, {-74.45, 92.45}}});
    s.lvBody[L7_REDWALL] = s.CreateBody("redWall", "Polygon", 0, F, R, {{{1.5, -61.5}, {1.5, 61.5}, {-18.5, 61.5}, {-18.5, -61.5}}});
    if (s.staticFlag[0]) LevelDestroyBody(s, s.lvBody[L7_REDWALL]);  // redCheckLevel
    const int32_t big = s.CreateBody("bigMovePlatform", "Polygon", D, F, R, {{{-75, -5}, {75, -5}, {75, 5}, {-75, 5}}});
    s.CreateBody("endPlatform", "Polygon", 0, F, R, {{{150, 0}, {150, 20}, {0, 20}, {0, 0}}});
    for (int k = 1; k <= 18; ++k) {
        char name[16];
        std::snprintf(name, sizeof name, "box%d", k);
        s.CreateBody(name, "Polygon", 0.3, F, R, {{{10, -10}, {10, 10}, {-10, 10}, {-10, -10}}});
    }
    s.lvBody[L7_BLUEPLAT] = s.CreateBody("bluePlatform", "Polygon", 0, F, R, {{{140, 0}, {140, 20}, {0, 20}, {0, 0}}});
    const int32_t star = s.CreateBody("star", "Polygon", D, F, R,
                                      {{{-0.25, 15}, {2, 27.85}, {-2.15, 27.95}},
                                       {{2, 27.85}, {15, 29.75}, {2.1, 32}, {-2.05, 32.1}, {-15, 30.2}, {-2.15, 27.95}},
                                       {{0.2, 45}, {-2.05, 32.1}, {2.1, 32}}});
    s.lvBody[L7_STAR] = star;
    w.SetAngularVelocity(star, -4);
    JointDef dj;
    w.InitDistanceJointDef(dj, big, w.groundBody, Vec2(3077 / PHYS_SCALE, -190 / PHYS_SCALE), Vec2(3005 / PHYS_SCALE, -305 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, big, w.groundBody, Vec2(2944 / PHYS_SCALE, -190 / PHYS_SCALE), Vec2(2873 / PHYS_SCALE, -305 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, star, potolok, Vec2(619 / PHYS_SCALE, -51.5 / PHYS_SCALE), Vec2(552 / PHYS_SCALE, -111 / PHYS_SCALE));
    w.CreateJoint(dj);
    JointDef pj;  // one b2PrismaticJointDef reused
    for (int k : {L7_JP1, L7_JP2}) {
        const int32_t jb = s.lvBody[k];
        w.InitPrismaticJointDef(pj, jb, w.groundBody, w.bodies[jb].sweep.c, Vec2(0, 1));
        pj.lowerTranslation = 0;
        pj.upperTranslation = 1;
        pj.enableLimit = true;
        pj.maxMotorForce = 0;
        pj.motorSpeed = 200;
        pj.enableMotor = true;
        s.lvInt[k == L7_JP1 ? L7I_JP1JOINT : L7I_JP2JOINT] = w.CreateJoint(pj);
    }
    w.InitPrismaticJointDef(pj, s.lvBody[L7_MP1], w.groundBody, w.bodies[s.lvBody[L7_MP1]].sweep.c, Vec2(0, 1));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L7I_MP1DIR] = 1;
    w.InitPrismaticJointDef(pj, s.lvBody[L7_MP2], w.groundBody, w.bodies[s.lvBody[L7_MP2]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L7I_MP2DIR] = 1;
}
static void L7_Switches(Sim& s) {
    if (s.switchFrame[0] == 1 && SwitchHit(s, "redCheck")) {
        s.switchFrame[0] = 2;
        LevelDestroyBody(s, s.lvBody[L7_REDWALL]);
        s.staticFlag[0] = true;  // Level_7.redCheckLevel
    }
    if (s.switchFrame[1] == 1 && SwitchHit(s, "blueCheck")) {
        s.switchFrame[1] = 2;
        LevelDestroyBody(s, s.lvBody[L7_BLUEPLAT]);
    }
}
static void L7_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 500 || PlayerTouches(s, s.lvBody[L7_STAR])) && s.playerAlive) s.PlayerDie();
    w.SetMaxMotorForce(s.lvInt[L7I_JP1JOINT], PlayerTouches(s, s.lvBody[L7_JP1]) ? 150 : 0);
    w.SetMaxMotorForce(s.lvInt[L7I_JP2JOINT], PlayerTouches(s, s.lvBody[L7_JP2]) ? 170 : 0);
    const int32_t m1 = s.lvBody[L7_MP1], m2 = s.lvBody[L7_MP2];
    if (s.spriteY[m1] < -30) s.lvInt[L7I_MP1DIR] = 1;
    if (s.spriteY[m1] > 115) s.lvInt[L7I_MP1DIR] = -1;
    w.SetLinearVelocity(m1, Vec2(0, 3 * s.lvInt[L7I_MP1DIR]));
    if (s.spriteX[m2] < 1495) s.lvInt[L7I_MP2DIR] = 1;
    if (s.spriteX[m2] > 1645) s.lvInt[L7I_MP2DIR] = -1;
    w.SetLinearVelocity(m2, Vec2(3 * s.lvInt[L7I_MP2DIR], 0));
    L7_Switches(s);
}

// Level_12.as: collision group -1 on the main platform and the kill star (prismatic, patrols in x),
// a little cart on two rollers held by distance joints.
enum { L12_STAR = 0 };
enum { L12I_DIR = 0 };
static void L12_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    FilterData group;
    group.groupIndex = -1;
    s.CreateBody("steps", "Polygon", 0, F, R,
                 {{{256, 0}, {256, 18}, {164, 18}, {138, 0}}, {{112, 18}, {138, 18}, {164, 106}, {112, 37}},
                  {{87, 37}, {112, 37}, {164, 106}, {87, 55}}, {{62, 55}, {87, 55}, {62, 72}}, {{38.5, 72}, {62, 72}, {38.5, 88.5}},
                  {{62, 72}, {87, 55}, {164, 106}, {38.5, 88.5}}, {{0, 88.5}, {38.5, 88.5}, {164, 106}, {0, 106}},
                  {{138, 18}, {138, 0}, {164, 18}, {164, 106}}});
    const int32_t main = s.CreateBody(
        "mainPlatform", "Polygon", 0, F, R,
        {{{1962.2, 0}, {1962.2, 18.05}, {1817.45, 7.75}, {1817.45, 0}},
         {{1817.45, 7.75}, {1962.2, 18.05}, {1799.45, 15.75}, {1799.45, 7.75}},
         {{1799.45, 15.75}, {1962.2, 18.05}, {1883.2, 18.05}, {1779.95, 15.75}},
         {{1761.2, 24}, {1779.95, 24}, {1761.2, 31.5}}, {{1741.95, 31.5}, {1761.2, 31.5}, {1741.95, 39.25}},
         {{1761.2, 31.5}, {1779.95, 24}, {1883.2, 193.25}, {1741.95, 39.25}},
         {{1431.65, 39.25}, {1741.95, 39.25}, {1883.2, 193.25}, {1431.65, 127.4}},
         {{884.25, 127.4}, {1431.65, 127.4}, {1883.2, 193.25}, {884.25, 168.2}},
         {{533, 127.2}, {533, 168.2}, {511.5, 168.25}, {511.5, 127.2}},
         {{533, 168.2}, {884.25, 168.2}, {1883.2, 193.25}, {0, 193.25}, {511.5, 168.25}},
         {{0, 168.25}, {511.5, 168.25}, {0, 193.25}},
         {{1779.95, 24}, {1779.95, 15.75}, {1883.2, 18.05}, {1883.2, 193.25}}});
    w.SetBodyFilter(main, group);
    s.CreateBody("boxPlatform", "Polygon", 0, F, R, {{{100, 0}, {100, 18}, {0, 18}, {0, 0}}});
    for (int k = 0; k < 3; ++k) {
        char name[16];
        std::snprintf(name, sizeof name, "box%d", k);
        s.CreateBody(name, "Polygon", D / 5, F, R, {{{20, 0}, {20, 20}, {0, 20}, {0, 0}}});
    }
    s.CreateBody("ceil", "Polygon", 0, F, R, {{{184, 0}, {184, 140.25}, {0, 140.25}, {0, 0}}});
    s.CreateBody("bigBox", "Polygon", D / 8, F, R, {{{0, 0}, {40, 0}, {40, 40}, {0, 40}}});
    const int32_t star = s.CreateCircleBody("killStar", D, F, R, 40);
    s.lvBody[L12_STAR] = star;
    w.SetBodyFilter(star, group);
    s.CreateBody("rampa", "Polygon", 0, 0.1, R,
                 {{{11.6, -14.25}, {11.6, -8.2}, {5.6, -8.2}, {0, -14.25}}, {{435.5, 84.5}, {354.5, 64.5}, {451, 87.5}},
                  {{354.5, 64.5}, {210.95, 30}, {505.5, 94}, {483.5, 92.5}, {451, 87.5}},
                  {{210.95, 30}, {157.95, 18}, {450, 81.5}, {505.5, 94}}, {{996, 70.6}, {1015, 70.6}, {1009, 76.6}, {996, 76.6}},
                  {{1015, 70.6}, {1015, 94}, {1009, 88.5}, {1009, 76.6}}, {{1015, 94}, {505.5, 94}, {507.5, 88.5}, {1009, 88.5}},
                  {{507.5, 88.5}, {505.5, 94}, {483, 86.5}}, {{483, 86.5}, {505.5, 94}, {450, 81.5}},
                  {{450, 81.5}, {157.95, 18}, {420, 74.5}}, {{420, 74.5}, {157.95, 18}, {211, 23.5}, {353.5, 57.5}},
                  {{157.95, 18}, {110, 12}, {158, 11.5}, {211, 23.5}}, {{110, 12}, {58.75, 7}, {110, 5.5}, {158, 11.5}},
                  {{58.75, 7}, {0, 7}, {5.6, 0}, {58.75, 0}, {110, 5.5}}, {{0, 7}, {0, -14.25}, {5.6, -8.2}, {5.6, 0}}});
    const int32_t r1 = s.CreateCircleBody("roll1", D, 0.1, R, 8);
    const int32_t r2 = s.CreateCircleBody("roll2", D, 0.1, R, 8);
    const int32_t go = s.CreateBody("goPlat", "Polygon", D, F, R,
                                    {{{5.45, 0.75}, {5.45, 10.1}, {0, 15.75}, {0, 0.75}},
                                     {{5.45, 10.1}, {54.05, 10.1}, {59.75, 15.75}, {0, 15.75}},
                                     {{54.05, 10.1}, {54.05, 0}, {59.75, 0}, {59.75, 15.75}}});
    JointDef pj;
    const Vec2 sp = w.bodies[star].xf.position;
    w.InitPrismaticJointDef(pj, star, w.groundBody, Vec2(sp.x + 20 / PHYS_SCALE, sp.y + 20 / PHYS_SCALE), Vec2(1, 0));
    w.CreateJoint(pj);
    JointDef dj;
    const Vec2 gp = w.bodies[go].xf.position, p1 = w.bodies[r1].xf.position, p2 = w.bodies[r2].xf.position;
    w.InitDistanceJointDef(dj, go, r1, Vec2(gp.x + 3 / PHYS_SCALE, gp.y + 1 / PHYS_SCALE), Vec2(p1.x + 4 / PHYS_SCALE, p1.y + 4 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, go, r2, Vec2(gp.x + 57 / PHYS_SCALE, gp.y + 1 / PHYS_SCALE), Vec2(p2.x + 4 / PHYS_SCALE, p2.y + 4 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, r1, r2, Vec2(p1.x + 4 / PHYS_SCALE, p1.y + 4 / PHYS_SCALE), Vec2(p2.x + 4 / PHYS_SCALE, p2.y + 4 / PHYS_SCALE));
    w.CreateJoint(dj);
    s.lvInt[L12I_DIR] = 1;  // killStarDirection:int = 1
}
static void L12_Update(Sim& s) {
    if ((s.spriteY[s.playerBody] > 500 || PlayerTouches(s, s.lvBody[L12_STAR])) && s.playerAlive) s.PlayerDie();
    const int32_t st = s.lvBody[L12_STAR];
    if (s.spriteX[st] < -1050) s.lvInt[L12I_DIR] = 1;
    if (s.spriteX[st] > -824) s.lvInt[L12I_DIR] = -1;
    s.world.SetLinearVelocity(st, Vec2(4 * s.lvInt[L12I_DIR], 0));
}

// Level_14.as: moving platform, catapult (revolute on a static pivot), blue switch, drop platform,
// a rolling kill ball kicked once the ball passes x = 1410.
enum { L14_MP1 = 0, L14_BLUEPLAT = 1, L14_DROP = 2, L14_ROLL = 3 };
enum { L14I_DIR = 0, L14I_ROLLGO = 1 };
static const PolyList kL14Drop = {{{-62, 5}, {-62, -5}, {62, -5}, {62, 5}}};
static void L14_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstPlatform", "Polygon", 0, F, R, {{{140, 10}, {-110, 10}, {-110, -10}, {140, -10}}});
    s.lvBody[L14_MP1] = s.CreateBody("movePlatform1", "Polygon", D, F, R,
                                     {{{-60, -5.35}, {-52.1, -17.1}, {-52.1, -5}, {-60, 5}}, {{60, 5}, {-60, 5}, {-52.1, -5}, {60, -5}}});
    s.CreateBody("catapultPlat", "Polygon", 0, F, R,
                 {{{94.8, -44}, {247.5, -44}, {-16.1, -12.95}}, {{-247.5, -12.95}, {-16.1, -12.95}, {-247.5, 44}},
                  {{-16.1, -12.95}, {247.5, -44}, {247.5, 44}, {-247.5, 44}}});
    s.CreateCircleBody("ball40", 2, 0.2, R, 45);
    const int32_t opora = s.CreateBody("catapultOpora", "Polygon", 0, F, R, {{{-0.25, -4.75}, {18.25, 24.75}, {-18.25, 24.75}}});
    const int32_t cat = s.CreateBody(
        "catapult", "Polygon", 0.5, F, R,
        {{{-72.05, -16.75}, {-71.3, -10.4}, {-77.25, -16.75}},
         {{-71.3, -10.4}, {-69.4, -5.75}, {-70.25, 1}, {-74.25, -3.65}, {-76.55, -10.1}, {-77.25, -16.75}},
         {{-70.25, 1}, {-69.4, -5.75}, {-65.15, -3}, {-65, 3}}, {{54.4, 3}, {-65, 3}, {-46.65, -3}, {54.4, -3}},
         {{-46.65, -3}, {-65, 3}, {-53.8, -3}, {-44.1, -7.1}}, {{-42.6, -11.9}, {-44.1, -7.1}, {-47.15, -13.55}, {-47.15, -17}, {-41.9, -17}},
         {{-47.15, -13.55}, {-44.1, -7.1}, {-48.15, -9.7}}, {{-48.15, -9.7}, {-44.1, -7.1}, {-50.25, -5.75}},
         {{-50.25, -5.75}, {-44.1, -7.1}, {-53.8, -3}}, {{-53.8, -3}, {-65, 3}, {-60, -2.25}}, {{-60, -2.25}, {-65, 3}, {-65.15, -3}}});
    s.CreateBody("floor", "Polygon", 0, F, R, {{{-199.9, 9}, {-199.9, -9}, {257.1, -9}, {257.1, 9}}});
    s.lvBody[L14_BLUEPLAT] = s.CreateBody("bluePlat", "Polygon", 0, F, R,
                                          {{{-44.8, -5}, {39.05, -5}, {47.6, 5}, {-44.8, 5}}, {{39.05, -5}, {39.05, -17.3}, {47.6, -4.9}, {47.6, 5}}});
    s.CreateBody("checkPlat", "Polygon", 0, F, R, {{{158, -10}, {158, 10}, {-62, 10}, {-62, -10}}});
    s.lvBody[L14_DROP] = s.CreateBody("dropPlat", "Polygon", 0, F, R, kL14Drop);
    s.CreateBody("finishPlat", "Polygon", 0, F, R,
                 {{{79.9, -10}, {162.5, -10}, {162.5, 83.5}, {17.9, 27.1}}, {{-135.25, 27.1}, {17.9, 27.1}, {162.5, 83.5}, {-118.25, 59.5}},
                  {{-250.45, 59.5}, {-118.25, 59.5}, {162.5, 83.5}, {-250.45, 79.5}}});
    s.lvBody[L14_ROLL] = s.CreateCircleBody("killRollBall", D, F, R, 50);
    s.CreateBody("barier", "Polygon", 0, F, R, {{{0, 0}, {20, 0}, {20, 50}, {0, 50}}});
    JointDef pj;
    w.InitPrismaticJointDef(pj, s.lvBody[L14_MP1], w.groundBody, w.bodies[s.lvBody[L14_MP1]].sweep.c, Vec2(0, 1));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L14I_DIR] = 1;
    JointDef rj;
    w.InitRevoluteJointDef(rj, cat, opora, w.bodies[cat].xf.position);
    w.CreateJoint(rj);
    s.lvInt[L14I_ROLLGO] = 0;
}
static void L14_Switches(Sim& s) {
    if (s.switchFrame[0] == 1 && SwitchHit(s, "blueCheck")) {
        s.switchFrame[0] = 2;
        LevelDestroyBody(s, s.lvBody[L14_BLUEPLAT]);
    }
}
static void L14_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 750 || PlayerTouches(s, s.lvBody[L14_ROLL])) && s.playerAlive) s.PlayerDie();
    const int32_t mp = s.lvBody[L14_MP1];
    if (s.spriteY[mp] < 232) s.lvInt[L14I_DIR] = 1;
    if (s.spriteY[mp] > 640) s.lvInt[L14I_DIR] = -1;
    w.SetLinearVelocity(mp, Vec2(0, 3 * s.lvInt[L14I_DIR]));
    L14_Switches(s);
    if (PlayerTouches(s, s.lvBody[L14_DROP])) s.lvBody[L14_DROP] = RecreateBody(s, s.lvBody[L14_DROP], "dropPlat", DEFAULT_DENSITY, kL14Drop);
    if (s.spriteX[s.playerBody] > 1410 && !s.lvInt[L14I_ROLLGO]) {
        s.lvInt[L14I_ROLLGO] = 1;
        const int32_t rb = s.lvBody[L14_ROLL];
        w.ApplyImpulse(rb, Vec2(-5, 0), w.bodies[rb].xf.position);
    }
}

// Level_13.as: moving platforms (prismatic patrols), two spinning kill stars on distance joints, a loose heavy star
// (kingStar1: a dynamic body without any joint, it falls and kills on touch), green switch removing a barrier.
// greenCheckLevel is static: after R the barrier is destroyed in the constructor, but greenCheck is still armed
// (greenCheck.stop() -> frame 1), so touching it again runs Level.DestroyBody on a body that is already gone
// (a no-op: Level.DestroyBody walks m_bodyList first).
enum { L13_KINGSTAR = 0, L13_MP1 = 1, L13_MP2 = 2, L13_BARIER = 3, L13_STAR1 = 4, L13_STAR2 = 5 };
enum { L13I_MP1DIR = 0, L13I_MP2DIR = 1 };
static void L13_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("mainPlat", "Polygon", 0, F, R, kL13_mainPlat);
    s.lvBody[L13_KINGSTAR] = s.CreateBody("kingStar1", "Polygon", 2 * D, F, R, kL13_kingStar1);
    s.lvBody[L13_MP1] = s.CreateBody("movePlat1", "Polygon", D, F, R, kL13_movePlat1);
    s.lvBody[L13_MP2] = s.CreateBody("movePlat2", "Polygon", D, F, R, kL13_movePlat2);
    s.CreateBody("skyLeft", "Polygon", 0, F, R, kL13_skyLeft);
    s.CreateBody("skyRight", "Polygon", 0, F, R, kL13_skyRight);
    s.lvBody[L13_BARIER] = s.CreateBody("greenBarier", "Polygon", 0, F, R, kL13_greenBarier);
    s.CreateBody("pereval", "Polygon", 0, F, R, kL13_pereval);
    s.CreateBody("pereval2", "Polygon", 0, F, R, kL13_pereval2);
    s.lvBody[L13_STAR1] = s.CreateBody("killStar1", "Polygon", D, F, R, kL13_killStar1);
    s.lvBody[L13_STAR2] = s.CreateBody("killStar2", "Polygon", D, F, R, kL13_killStar2);
    if (s.staticFlag[1]) LevelDestroyBody(s, s.lvBody[L13_BARIER]);  // greenCheckLevel
    JointDef pj, dj;
    w.InitPrismaticJointDef(pj, s.lvBody[L13_MP1], w.groundBody, w.bodies[s.lvBody[L13_MP1]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L13I_MP1DIR] = -1;
    w.InitPrismaticJointDef(pj, s.lvBody[L13_MP2], w.groundBody, w.bodies[s.lvBody[L13_MP2]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L13I_MP2DIR] = -1;
    const Vec2 p1 = w.bodies[s.lvBody[L13_STAR1]].xf.position;
    w.InitDistanceJointDef(dj, s.lvBody[L13_STAR1], w.groundBody, p1, Vec2(p1.x - 100 / PHYS_SCALE, p1.y - 100 / PHYS_SCALE));
    w.CreateJoint(dj);
    const Vec2 p2 = w.bodies[s.lvBody[L13_STAR2]].xf.position;
    w.InitDistanceJointDef(dj, s.lvBody[L13_STAR2], w.groundBody, p2, Vec2(p2.x + 100 / PHYS_SCALE, p2.y - 100 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.SetAngularVelocity(s.lvBody[L13_STAR1], 10);
    w.SetAngularVelocity(s.lvBody[L13_STAR2], 10);
}
static void L13_Switches(Sim& s) {  // hitTestObject without IsLive(): also runs (dead-ball rule) after a death
    if (s.switchFrame[0] == 1 && SwitchHit(s, "greenCheck")) {
        s.switchFrame[0] = 2;
        s.staticFlag[1] = true;  // Level_13.greenCheckLevel
        LevelDestroyBody(s, s.lvBody[L13_BARIER]);
    }
}
static void L13_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 500 || PlayerTouches(s, s.lvBody[L13_KINGSTAR]) || PlayerTouches(s, s.lvBody[L13_STAR1]) ||
         PlayerTouches(s, s.lvBody[L13_STAR2])) && s.playerAlive)
        s.PlayerDie();
    const int32_t m1 = s.lvBody[L13_MP1], m2 = s.lvBody[L13_MP2];
    if (s.spriteX[m1] < -1237) s.lvInt[L13I_MP1DIR] = 1;
    if (s.spriteX[m1] > -863) s.lvInt[L13I_MP1DIR] = -1;
    w.SetLinearVelocity(m1, Vec2(3 * s.lvInt[L13I_MP1DIR], 0));
    if (s.spriteX[m2] < -572) s.lvInt[L13I_MP2DIR] = 1;
    if (s.spriteX[m2] > -68) s.lvInt[L13I_MP2DIR] = -1;
    w.SetLinearVelocity(m2, Vec2(3 * s.lvInt[L13I_MP2DIR], 0));
    L13_Switches(s);
}

// Level_16.as: patrolling platforms, a jump platform (prismatic limit + motor, motor force 250 while touched),
// three blue switches that each destroy the same barrier plate (the 2nd/3rd are Level.DestroyBody no-ops), a drop platform
// that is re-created dynamic with density 3, two kill roll balls, a swinging axe and the wrongWay trigger.
// wrongWay (hitTestObject, no IsLive() guard, so also after death) sets lastCheckNum = 0 AFTER Level.Update's checkpoint
// loop, and Level_16.isStrelka = true (static: survives R).
// Field initialisers: killRollBall1Direction = 1, killRollBall2Direction = -1.
enum { L16_MP1 = 0, L16_MP2 = 1, L16_JUMP = 2, L16_BLUEPLATE = 3, L16_DROP = 4, L16_BALL1 = 5, L16_BALL2 = 6, L16_AXE = 7 };
enum { L16I_MP1DIR = 0, L16I_MP2DIR = 1, L16I_BALL1DIR = 2, L16I_BALL2DIR = 3, L16I_JUMPJOINT = 4 };
static void L16_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstPlat", "Polygon", 0, F, R, kL16_firstPlat);
    s.CreateBody("plat2", "Polygon", 0, F, R, kL16_plat2);
    s.CreateBody("plat1", "Polygon", 0, F, R, kL16_plat1);
    s.lvBody[L16_MP1] = s.CreateBody("movePlatform1", "Polygon", D, F, R, kL16_movePlatform1);
    s.lvBody[L16_MP2] = s.CreateBody("movePlatform2", "Polygon", D, F, R, kL16_movePlatform2);
    s.lvBody[L16_JUMP] = s.CreateBody("jumpPlatform1", "Polygon", D, F, R, kL16_jumpPlatform1);
    s.CreateBody("dangerPlato", "Polygon", 0, F, R, kL16_dangerPlato);
    s.lvBody[L16_BLUEPLATE] = s.CreateBody("bluePlato", "Polygon", 0, F, R, kL16_bluePlato);
    s.lvBody[L16_DROP] = s.CreateBody("dropPlatform", "Polygon", 0, F, R, kL16_dropPlatform);
    s.lvBody[L16_BALL1] = s.CreateCircleBody("killRollBall1", D, F, R, 50);
    s.lvBody[L16_BALL2] = s.CreateCircleBody("killRollBall2", D, F, R, 50);
    s.CreateBody("finishPlato", "Polygon", 0, F, R, kL16_finishPlato);
    s.lvBody[L16_AXE] = s.CreateBody("axe1", "Polygon", 2 * D, F, R, kL16_axe1);
    JointDef pj;  // one b2PrismaticJointDef reused
    w.InitPrismaticJointDef(pj, s.lvBody[L16_MP1], w.groundBody, w.bodies[s.lvBody[L16_MP1]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L16I_MP1DIR] = 1;
    w.InitPrismaticJointDef(pj, s.lvBody[L16_MP2], w.groundBody, w.bodies[s.lvBody[L16_MP2]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L16I_MP2DIR] = 1;
    w.InitPrismaticJointDef(pj, s.lvBody[L16_JUMP], w.groundBody, w.bodies[s.lvBody[L16_JUMP]].sweep.c, Vec2(0, 1));
    pj.lowerTranslation = 0;
    pj.upperTranslation = 1;
    pj.enableLimit = true;
    pj.maxMotorForce = 0;
    pj.motorSpeed = 200;
    pj.enableMotor = true;
    s.lvInt[L16I_JUMPJOINT] = w.CreateJoint(pj);
    JointDef rj;
    w.InitRevoluteJointDef(rj, s.lvBody[L16_AXE], w.groundBody, Vec2(2767 / PHYS_SCALE, -60 / PHYS_SCALE));
    w.CreateJoint(rj);
    s.lvInt[L16I_BALL1DIR] = 1;   // private var killRollBall1Direction:int = 1
    s.lvInt[L16I_BALL2DIR] = -1;  // private var killRollBall2Direction:int = -1
}
static void L16_Switches(Sim& s) {  // unguarded hit tests (alive or dead ball)
    if (SwitchHit(s, "wrongWay")) {
        s.lastCheckNum = 0;
        s.staticFlag[2] = true;  // Level_16.isStrelka
    }
    static const char* blue[3] = {"blueCheck1", "blueCheck2", "blueCheck3"};
    for (int k = 0; k < 3; ++k)
        if (s.switchFrame[k] == 1 && SwitchHit(s, blue[k])) {
            s.switchFrame[k] = 2;
            LevelDestroyBody(s, s.lvBody[L16_BLUEPLATE]);
        }
}
static void L16_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 1000 || PlayerTouches(s, s.lvBody[L16_BALL1]) || PlayerTouches(s, s.lvBody[L16_BALL2]) ||
         PlayerTouches(s, s.lvBody[L16_AXE])) && s.playerAlive)
        s.PlayerDie();
    if (SwitchHit(s, "wrongWay")) {
        s.lastCheckNum = 0;
        s.staticFlag[2] = true;
    }
    const int32_t m1 = s.lvBody[L16_MP1], m2 = s.lvBody[L16_MP2];
    if (s.spriteX[m1] < 457) s.lvInt[L16I_MP1DIR] = 1;
    if (s.spriteX[m1] > 787) s.lvInt[L16I_MP1DIR] = -1;
    w.SetLinearVelocity(m1, Vec2(3 * s.lvInt[L16I_MP1DIR], 0));
    if (s.spriteX[m2] < 999) s.lvInt[L16I_MP2DIR] = 1;
    if (s.spriteX[m2] > 1350) s.lvInt[L16I_MP2DIR] = -1;
    w.SetLinearVelocity(m2, Vec2(3 * s.lvInt[L16I_MP2DIR], 0));
    w.SetMaxMotorForce(s.lvInt[L16I_JUMPJOINT], PlayerTouches(s, s.lvBody[L16_JUMP]) ? 250 : 0);
    static const char* blue[3] = {"blueCheck1", "blueCheck2", "blueCheck3"};
    for (int k = 0; k < 3; ++k)
        if (s.switchFrame[k] == 1 && SwitchHit(s, blue[k])) {
            s.switchFrame[k] = 2;
            LevelDestroyBody(s, s.lvBody[L16_BLUEPLATE]);
        }
    if (PlayerTouches(s, s.lvBody[L16_DROP])) s.lvBody[L16_DROP] = RecreateBody(s, s.lvBody[L16_DROP], "dropPlatform", 3 * DEFAULT_DENSITY, kL16_dropPlatform);
    const int32_t b1 = s.lvBody[L16_BALL1], b2 = s.lvBody[L16_BALL2];
    if (s.spriteX[b1] < 1450) s.lvInt[L16I_BALL1DIR] = 1;
    if (s.spriteX[b1] > 1750) s.lvInt[L16I_BALL1DIR] = -1;
    w.SetAngularVelocity(b1, 5 * s.lvInt[L16I_BALL1DIR]);
    if (s.spriteX[b2] < 2120) s.lvInt[L16I_BALL2DIR] = 1;
    if (s.spriteX[b2] > 2495) s.lvInt[L16I_BALL2DIR] = -1;
    w.SetAngularVelocity(b2, 5 * s.lvInt[L16I_BALL2DIR]);
}

// Level_9.as: three-bar crank (motorised revolute -> two revolute links -> boom with a prismatic guide), a ten-plank
// bridge pinned at both ends, a patrolling green platform removed by any of three green switches, a kill roll ball and three
// pendulum jump balls on distance joints. Field initialisers: greenPlatformBodyDirection = 1, killRollBallDirection = 1.
// Three joint defs are reused exactly as in the AS3 (the crank def keeps motorSpeed/maxMotorTorque after
// enableMotor = false, which is harmless because the motor is off).
enum { L9_GREEN = 0, L9_BOOM = 1, L9_ROLL = 2 };
enum { L9I_GREENDIR = 0, L9I_ROLLDIR = 1 };
static void L9_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    const int32_t crank1 = s.CreateBody("firstCrank", "Polygon", D, F, R, kL9_firstCrank);
    const int32_t crank2 = s.CreateBody("secondCrank", "Polygon", D, F, R, kL9_secondCrank);
    const int32_t boom = s.lvBody[L9_BOOM] = s.CreateBody("boomCrank", "Polygon", D, F, R, kL9_boomCrank);
    s.CreateBody("firstBigPlatform", "Polygon", 0, F, R, kL9_firstBigPlatform);
    const int32_t green = s.lvBody[L9_GREEN] = s.CreateBody("greenPlatform", "Polygon", D, F, R, kL9_greenPlatform);
    const int32_t low = s.CreateBody("lowPlatform", "Polygon", 0, F, R, kL9_lowPlatform);
    const int32_t low2 = s.CreateBody("low2Platform", "Polygon", 0, F, R, kL9_low2Platform);
    s.CreateBody("upPlatform", "Polygon", 0, F, R, kL9_upPlatform);
    s.CreateBody("middlePlatform", "Polygon", 0, F, R, kL9_middlePlatform);
    s.CreateBody("finishPlatform", "Polygon", 0, F, R, kL9_finishPlatform);
    const PolyList* bridgePolys[10] = {&kL9_bridgeElement1, &kL9_bridgeElement2, &kL9_bridgeElement3, &kL9_bridgeElement4,
                                       &kL9_bridgeElement5, &kL9_bridgeElement6, &kL9_bridgeElement7, &kL9_bridgeElement8,
                                       &kL9_bridgeElement9, &kL9_bridgeElement10};
    int32_t bridge[10];
    for (int k = 0; k < 10; ++k) {
        char name[24];
        std::snprintf(name, sizeof name, "bridgeElement%d", k + 1);
        bridge[k] = s.CreateBody(name, "Polygon", D, F, R, *bridgePolys[k]);
    }
    s.lvBody[L9_ROLL] = s.CreateCircleBody("killRollBall", D, F, R, 50);
    const int32_t jb1 = s.CreateCircleBody("jumpBall1", D, F, R, 75);
    const int32_t jb2 = s.CreateCircleBody("jumpBall2", D, F, R, 75);
    const int32_t jb3 = s.CreateCircleBody("jumpBall3", D, F, R, 75);
    JointDef dj, rj, pj;
    w.InitPrismaticJointDef(pj, green, w.groundBody, w.bodies[green].sweep.c, Vec2(1, 0));
    w.CreateJoint(pj);
    w.InitRevoluteJointDef(rj, low, bridge[0], w.bodies[bridge[0]].xf.position);
    w.CreateJoint(rj);
    for (int k = 0; k < 9; ++k) {
        w.InitRevoluteJointDef(rj, bridge[k], bridge[k + 1], w.bodies[bridge[k + 1]].xf.position);
        w.CreateJoint(rj);
    }
    const Vec2 b10 = w.bodies[bridge[9]].xf.position;
    w.InitRevoluteJointDef(rj, bridge[9], low2, Vec2(b10.x + 50 / PHYS_SCALE, b10.y));
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, crank1, w.groundBody, w.bodies[crank1].xf.position);
    rj.motorSpeed = 1.8 * AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    rj.enableMotor = false;
    w.InitRevoluteJointDef(rj, crank2, crank1, w.bodies[crank2].xf.position);
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, boom, crank2, w.bodies[boom].xf.position);
    w.CreateJoint(rj);
    w.InitPrismaticJointDef(pj, boom, w.groundBody, w.bodies[boom].sweep.c, Vec2(1, 0));
    w.CreateJoint(pj);
    w.InitDistanceJointDef(dj, jb1, w.groundBody, Vec2(2236 / PHYS_SCALE, 63 / PHYS_SCALE), Vec2(2172 / PHYS_SCALE, -56 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, jb2, w.groundBody, Vec2(2370 / PHYS_SCALE, 63 / PHYS_SCALE), Vec2(2435 / PHYS_SCALE, -56 / PHYS_SCALE));
    w.CreateJoint(dj);
    w.InitDistanceJointDef(dj, jb3, w.groundBody, Vec2(2761 / PHYS_SCALE, 63 / PHYS_SCALE), Vec2(2698 / PHYS_SCALE, -56 / PHYS_SCALE));
    w.CreateJoint(dj);
    s.lvInt[L9I_GREENDIR] = 1;  // private var greenPlatformBodyDirection:int = 1
    s.lvInt[L9I_ROLLDIR] = 1;   // private var killRollBallDirection:int = 1
}
static void L9_Switches(Sim& s) {  // greenCheck1..3: each destroys the same platform (2nd/3rd are no-ops)
    static const char* green[3] = {"greenCheck1", "greenCheck2", "greenCheck3"};
    for (int k = 0; k < 3; ++k)
        if (s.switchFrame[k] == 1 && SwitchHit(s, green[k])) {
            s.switchFrame[k] = 2;
            LevelDestroyBody(s, s.lvBody[L9_GREEN]);
        }
}
static void L9_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 530 || PlayerTouches(s, s.lvBody[L9_ROLL]) || PlayerTouches(s, s.lvBody[L9_BOOM])) && s.playerAlive)
        s.PlayerDie();
    const int32_t g = s.lvBody[L9_GREEN];
    if (s.spriteX[g] < 55) s.lvInt[L9I_GREENDIR] = 1;
    if (s.spriteX[g] > 387) s.lvInt[L9I_GREENDIR] = -1;
    w.SetLinearVelocity(g, Vec2(2 * s.lvInt[L9I_GREENDIR], 0));
    L9_Switches(s);
    const int32_t r = s.lvBody[L9_ROLL];
    if (s.spriteX[r] < 740) s.lvInt[L9I_ROLLDIR] = 1;
    if (s.spriteX[r] > 1200) s.lvInt[L9I_ROLLDIR] = -1;
    w.SetAngularVelocity(r, 5 * s.lvInt[L9I_ROLLDIR]);
}

// Level_10.as: three jump platforms (prismatic limit + motor, 150 while touched), three motorised cubes, a round kill
// block spinning about a point away from its body, a heavy half-density ball; afterJump is a rotated static body.
enum { L10_JP1 = 0, L10_JP2 = 1, L10_JP3 = 2, L10_BLOCK = 3 };
enum { L10I_J1 = 0, L10I_J2 = 1, L10I_J3 = 2 };
static void L10_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("firstPlatform", "Polygon", 0, F, R, kL10_firstPlatform);
    s.CreateBody("barierPlatform", "Polygon", 0, F, R, kL10_barierPlatform);
    s.CreateBody("firstBarier", "Polygon", 0, F, R, kL10_firstBarier);
    s.CreateBody("secondBarier", "Polygon", 0, F, R, kL10_secondBarier);
    s.CreateBody("thirdBarier", "Polygon", 0, F, R, kL10_thirdBarier);
    s.CreateBody("forthBarier", "Polygon", 0, F, R, kL10_forthBarier);
    s.lvBody[L10_JP1] = s.CreateBody("jumpPlatform1", "Polygon", D, F, R, kL10_jumpPlatform1);
    s.lvBody[L10_JP2] = s.CreateBody("jumpPlatform2", "Polygon", D, F, R, kL10_jumpPlatform2);
    s.lvBody[L10_JP3] = s.CreateBody("jumpPlatform3", "Polygon", D, F, R, kL10_jumpPlatform3);
    s.CreateBody("afterJump", "Polygon", 0, F, R, kL10_afterJump);
    const int32_t c1 = s.CreateBody("cube1", "Polygon", D, F, R, kL10_cube1);
    const int32_t c2 = s.CreateBody("cube2", "Polygon", D, F, R, kL10_cube2);
    const int32_t c3 = s.CreateBody("cube3", "Polygon", D, F, R, kL10_cube3);
    s.CreateBody("mainRampa", "Polygon", 0, F, R, kL10_mainRampa);
    s.CreateCircleBody("goBall", D / 2, F, R, 80);
    s.lvBody[L10_BLOCK] = s.CreateBody("roundBlock", "Polygon", D, F, R, kL10_roundBlock);
    JointDef pj, rj;
    for (int k = 0; k < 3; ++k) {
        const int32_t jb = s.lvBody[L10_JP1 + k];
        w.InitPrismaticJointDef(pj, jb, w.groundBody, w.bodies[jb].sweep.c, Vec2(0, 1));
        pj.lowerTranslation = 0;
        pj.upperTranslation = 1;
        pj.enableLimit = true;
        pj.maxMotorForce = 0;
        pj.motorSpeed = 200;
        pj.enableMotor = true;
        s.lvInt[L10I_J1 + k] = w.CreateJoint(pj);
    }
    w.InitRevoluteJointDef(rj, c1, w.groundBody, w.bodies[c1].xf.position);
    rj.motorSpeed = 0.3 * AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, c2, w.groundBody, w.bodies[c2].xf.position);
    rj.motorSpeed = -0.3 * AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, c3, w.groundBody, w.bodies[c3].xf.position);
    rj.motorSpeed = 0.3 * AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, s.lvBody[L10_BLOCK], w.groundBody, Vec2(-675 / PHYS_SCALE, -152 / PHYS_SCALE));
    rj.motorSpeed = -0.9 * AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
}
static void L10_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 500 || PlayerTouches(s, s.lvBody[L10_BLOCK])) && s.playerAlive) s.PlayerDie();
    for (int k = 0; k < 3; ++k)
        w.SetMaxMotorForce(s.lvInt[L10I_J1 + k], PlayerTouches(s, s.lvBody[L10_JP1 + k]) ? 150 : 0);
}

// Level_17.as: the crown is two static bodies (kingCrown1 carries the star's revolute joint), a jump platform (motor force
// 150 while touched), a spinning star, a static kill triangle and two patrolling platforms.
// Quirk reproduced: the constructor sets movePlatform1BodyDirection twice (1, then -1 after movePlatform2's joint) and
// never initialises movePlatform2BodyDirection, so platform 2 starts with direction 0: SetLinearVelocity(0, 0) each
// frame, gravity sinks it along its axis a little each frame, until its y passes 430 and it turns (y starts at 430).
enum { L17_JUMP = 0, L17_KILL = 1, L17_STAR = 2, L17_MP1 = 3, L17_MP2 = 4 };
enum { L17I_MP1DIR = 0, L17I_MP2DIR = 1, L17I_JUMPJOINT = 2 };
static void L17_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    const int32_t crown1 = s.CreateBody("kingCrown1", "Polygon", 0, F, R, kL17_kingCrown1);
    s.CreateBody("kingCrown2", "Polygon", 0, F, R, kL17_kingCrown2);
    s.lvBody[L17_JUMP] = s.CreateBody("jumpPlatform1", "Polygon", D, F, R, kL17_jumpPlatform1);
    s.lvBody[L17_KILL] = s.CreateBody("killStarPart", "Polygon", 0, F, R, kL17_killStarPart);
    s.lvBody[L17_STAR] = s.CreateBody("star", "Polygon", D, F, R, kL17_star);
    s.lvBody[L17_MP1] = s.CreateBody("movePlatform1", "Polygon", D, F, R, kL17_movePlatform1);
    s.lvBody[L17_MP2] = s.CreateBody("movePlatform2", "Polygon", D, F, R, kL17_movePlatform2);
    JointDef pj, rj;
    w.InitPrismaticJointDef(pj, s.lvBody[L17_JUMP], w.groundBody, w.bodies[s.lvBody[L17_JUMP]].sweep.c, Vec2(0, 1));
    pj.lowerTranslation = 0;
    pj.upperTranslation = 1;
    pj.enableLimit = true;
    pj.maxMotorForce = 0;
    pj.motorSpeed = 200;
    pj.enableMotor = true;
    s.lvInt[L17I_JUMPJOINT] = w.CreateJoint(pj);
    w.InitPrismaticJointDef(pj, s.lvBody[L17_MP1], w.groundBody, w.bodies[s.lvBody[L17_MP1]].sweep.c, Vec2(1, 0));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L17I_MP1DIR] = 1;
    w.InitPrismaticJointDef(pj, s.lvBody[L17_MP2], w.groundBody, w.bodies[s.lvBody[L17_MP2]].sweep.c, Vec2(0, 1));
    pj.enableLimit = false;
    pj.enableMotor = false;
    w.CreateJoint(pj);
    s.lvInt[L17I_MP1DIR] = -1;  // sic: movePlatform1BodyDirection again; movePlatform2BodyDirection stays 0
    s.lvInt[L17I_MP2DIR] = 0;
    w.InitRevoluteJointDef(rj, s.lvBody[L17_STAR], crown1, w.bodies[s.lvBody[L17_STAR]].xf.position);
    w.CreateJoint(rj);
}
static void L17_Update(Sim& s) {
    World& w = s.world;
    if ((s.spriteY[s.playerBody] > 960 || PlayerTouches(s, s.lvBody[L17_KILL])) && s.playerAlive) s.PlayerDie();
    w.SetMaxMotorForce(s.lvInt[L17I_JUMPJOINT], PlayerTouches(s, s.lvBody[L17_JUMP]) ? 150 : 0);
    w.SetAngularVelocity(s.lvBody[L17_STAR], -1);
    const int32_t m1 = s.lvBody[L17_MP1], m2 = s.lvBody[L17_MP2];
    if (s.spriteX[m1] < 903) s.lvInt[L17I_MP1DIR] = 1;
    if (s.spriteX[m1] > 1123) s.lvInt[L17I_MP1DIR] = -1;
    w.SetLinearVelocity(m1, Vec2(3 * s.lvInt[L17I_MP1DIR], 0));
    if (s.spriteY[m2] < 272) s.lvInt[L17I_MP2DIR] = 1;
    if (s.spriteY[m2] > 430) s.lvInt[L17I_MP2DIR] = -1;
    w.SetLinearVelocity(m2, Vec2(0, 3 * s.lvInt[L17I_MP2DIR]));
}

// Level_15.as: a patrolling platform (prismatic, y), a skateboard on two wheel joints, the loose plank "luk" (only
// built when lastCheckNum == 0), a red switch that destroys the gate and rebuilds it as a dynamic door hinged on
// shopLeftSide (spinning at 3 rad/s), and killLine: a thin MovieClip that sweeps left by 4 px per frame (reset to 387 below
// x = 73) and kills on hitTestObject. killLine.x is kept in twips in lvInt; its box is the display_data.h box moved with it.
enum { L15_MP = 0, L15_GATE = 1, L15_SHOPLEFT = 2 };
enum { L15I_MPDIR = 0, L15I_KILLX = 1 };
static void L15_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.CreateBody("rampa2", "Polygon", 0, F, R, kL15_rampa2);
    s.lvBody[L15_SHOPLEFT] = s.CreateBody("shopLeftSide", "Polygon", 0, F, R, kL15_shopLeftSide);
    s.CreateBody("shopRightSide", "Polygon", 0, F, R, kL15_shopRightSide);
    s.CreateBody("step1", "Polygon", 0, F, R, kL15_step1);
    s.CreateBody("step2", "Polygon", 0, F, R, kL15_step2);
    s.CreateBody("step3", "Polygon", 0, F, R, kL15_step3);
    s.lvBody[L15_GATE] = s.CreateBody("redGate", "Polygon", 0, F, R, kL15_redGate);
    s.CreateBody("finishPlat", "Polygon", 0, F, R, kL15_finishPlat);
    s.CreateBody("barier", "Polygon", 0, F, R, kL15_barier);
    s.lvBody[L15_MP] = s.CreateBody("movePlatform", "Polygon", D, F, R, kL15_movePlatform);
    s.CreateBody("underShop", "Polygon", 0, F, R, kL15_underShop);
    if (s.lastCheckNum == 0) s.CreateBody("luk", "Polygon", D / 3, F / 3, R, kL15_luk);
    const int32_t board = s.CreateBody("skateBoard1", "Polygon", D, F, R, kL15_skateBoard1);
    const int32_t wh1 = s.CreateCircleBody("skateWheel11", D, F, R, 6);
    const int32_t wh2 = s.CreateCircleBody("skateWheel12", D, F, R, 6);
    JointDef pj, rj;
    w.InitPrismaticJointDef(pj, s.lvBody[L15_MP], w.groundBody, w.bodies[s.lvBody[L15_MP]].sweep.c, Vec2(0, 1));
    w.CreateJoint(pj);
    s.lvInt[L15I_MPDIR] = 1;
    const Vec2 p1 = w.bodies[wh1].xf.position, p2 = w.bodies[wh2].xf.position;
    w.InitRevoluteJointDef(rj, board, wh1, Vec2(p1.x + 3 / PHYS_SCALE, p1.y + 3 / PHYS_SCALE));
    w.CreateJoint(rj);
    w.InitRevoluteJointDef(rj, board, wh2, Vec2(p2.x + 3 / PHYS_SCALE, p2.y + 3 / PHYS_SCALE));
    w.CreateJoint(rj);
    s.lvInt[L15I_KILLX] = s.tpl->Place("killLine").tx;  // twips
}
static bool L15_KillLineHit(Sim& s) {
    DisplayObj o = *s.tpl->Display("killLine");
    const double dx = s.lvInt[L15I_KILLX] - s.tpl->Place("killLine").tx;
    o.x0 += dx;
    o.x1 += dx;
    return s.BallHitsTarget(o);
}
static void L15_Switches(Sim& s) {
    if (s.switchFrame[0] == 1 && SwitchHit(s, "redCheck")) {
        s.switchFrame[0] = 2;
        World& w = s.world;
        s.lvBody[L15_GATE] = RecreateBody(s, s.lvBody[L15_GATE], "redGate", DEFAULT_DENSITY, kL15_redGate);
        JointDef rj;
        w.InitRevoluteJointDef(rj, s.lvBody[L15_SHOPLEFT], s.lvBody[L15_GATE], w.bodies[s.lvBody[L15_GATE]].xf.position);
        w.CreateJoint(rj);
        w.SetAngularVelocity(s.lvBody[L15_GATE], 3);
    }
}
static void L15_Update(Sim& s) {
    World& w = s.world;
    if (s.playerAlive && (s.spriteY[s.playerBody] > 860 || L15_KillLineHit(s))) s.PlayerDie();
    L15_Switches(s);
    const int32_t mp = s.lvBody[L15_MP];
    if (s.spriteY[mp] < 211) s.lvInt[L15I_MPDIR] = 1;
    if (s.spriteY[mp] > 368) s.lvInt[L15I_MPDIR] = -1;
    w.SetLinearVelocity(mp, Vec2(0, 3 * s.lvInt[L15I_MPDIR]));
    if (s.lvInt[L15I_KILLX] < 73 * 20)
        s.lvInt[L15I_KILLX] = 387 * 20;
    else
        s.lvInt[L15I_KILLX] -= 4 * 20;
    // money: hitTestObject only starts a scale tween (display only)
}

// Level_11.as: a train (heavy body, two motorised wheels, eleven wagons chained by distance joints, each on two free wheels),
// rotating and spinning kill bodies, loose kill logs, and levelAim as a DYNAMIC body: the goal flag can be pushed and its hit
// box follows the sprite (Sim::aimBody). killStarBody[3..5] in Level_11.Update read past the 3-element array
// (undefined, never in the contact list): harmless, not modelled.
enum { L11_KILLCEIL = 0, L11_KILLROTATE = 1, L11_STAR0 = 2, L11_BREVNO0 = 5 };
static void L11_Construct(Sim& s) {
    World& w = s.world;
    const double F = DEFAULT_FRICTION, R = DEFAULT_RESTITUTION, D = DEFAULT_DENSITY;
    s.aimBody = s.CreateBody("levelAim", "Polygon", D, F, R, kL11_levelAim);
    const int32_t train = s.CreateBody("train", "Polygon", 5 * D, F, R, kL11_train);
    const int32_t kt1 = s.CreateCircleBody("kolesoTrain1", D, F, R, 8);
    const int32_t kt2 = s.CreateCircleBody("kolesoTrain2", D, F, R, 8);
    int32_t vagon[11], koleso[11][2];
    for (int i = 1; i <= 11; ++i) {
        char name[24];
        std::snprintf(name, sizeof name, "vagon%d", i);
        const bool simple = i == 3 || i == 4 || i == 5 || i == 9;
        vagon[i - 1] = s.CreateBody(name, "Polygon", D, F, R, simple ? kL11_vagon_1 : kL11_vagon_2);
        std::snprintf(name, sizeof name, "koleso_%d_1", i);
        koleso[i - 1][0] = s.CreateCircleBody(name, D, F, R, 8);
        std::snprintf(name, sizeof name, "koleso_%d_2", i);
        koleso[i - 1][1] = s.CreateCircleBody(name, D, F, R, 8);
    }
    s.CreateBody("platform", "Polygon", 0, F, R, kL11_platform);
    s.CreateBody("ceil1", "Polygon", 0, F, R, kL11_ceil1);
    s.lvBody[L11_KILLCEIL] = s.CreateBody("killCeil1", "Polygon", 0, F, R, kL11_killCeil1);
    s.CreateBody("ceil2_1", "Polygon", 0, F, R, kL11_ceil2_1);
    s.CreateBody("ceil2_2", "Polygon", 0, F, R, kL11_ceil2_2);
    const int32_t rot = s.lvBody[L11_KILLROTATE] = s.CreateBody("killRotate", "Polygon", D, F, R, kL11_killRotate);
    for (int k = 0; k < 3; ++k) {
        char name[24];
        std::snprintf(name, sizeof name, "killStar%d", k);
        s.lvBody[L11_STAR0 + k] = s.CreateCircleBody(name, D, F, R, 40);
    }
    for (int k = 0; k < 4; ++k) {
        char name[24];
        std::snprintf(name, sizeof name, "killBrevno%d", k);
        s.lvBody[L11_BREVNO0 + k] = s.CreateBody(name, "Polygon", D, F, R, kL11_killBrevno_1);
    }
    s.CreateBody("triangle", "Polygon", 0, F, R, kL11_triangle);
    s.CreateBody("upCeil", "Polygon", 0, F, R, kL11_upCeil);
    JointDef dj, rj;
    w.InitRevoluteJointDef(rj, rot, w.groundBody, w.bodies[rot].xf.position);
    rj.motorSpeed = -AS3_PI;
    rj.maxMotorTorque = 5000;
    rj.enableMotor = true;
    w.CreateJoint(rj);
    for (int k = 0; k < 3; ++k) {
        const int32_t st = s.lvBody[L11_STAR0 + k];
        const Vec2 p = w.bodies[st].xf.position;
        w.InitRevoluteJointDef(rj, st, w.groundBody, Vec2(p.x + 20 / PHYS_SCALE, p.y + 20 / PHYS_SCALE));
        rj.motorSpeed = -5 * AS3_PI;
        rj.maxMotorTorque = 5000;
        rj.enableMotor = true;
        w.CreateJoint(rj);
    }
    for (const int32_t kt : {kt1, kt2}) {
        const Vec2 p = w.bodies[kt].xf.position;
        w.InitRevoluteJointDef(rj, train, kt, Vec2(p.x + 4 / PHYS_SCALE, p.y + 4 / PHYS_SCALE));
        rj.motorSpeed = 7 * AS3_PI;
        rj.maxMotorTorque = 5000;
        rj.enableMotor = true;
        w.CreateJoint(rj);
    }
    rj.enableMotor = false;
    for (int i = 0; i < 11; ++i)
        for (int j = 0; j < 2; ++j) {
            const Vec2 p = w.bodies[koleso[i][j]].xf.position;
            w.InitRevoluteJointDef(rj, vagon[i], koleso[i][j], Vec2(p.x + 4 / PHYS_SCALE, p.y + 4 / PHYS_SCALE));
            w.CreateJoint(rj);
        }
    w.InitDistanceJointDef(dj, train, vagon[0], w.bodies[train].xf.position, w.bodies[vagon[0]].xf.position);
    w.CreateJoint(dj);
    for (int i = 0; i < 10; ++i) {
        const Vec2 p = w.bodies[vagon[i]].xf.position;
        w.InitDistanceJointDef(dj, vagon[i], vagon[i + 1], Vec2(p.x - 75 / PHYS_SCALE, p.y), w.bodies[vagon[i + 1]].xf.position);
        w.CreateJoint(dj);
    }
}
static void L11_Update(Sim& s) {
    if (!s.playerAlive) return;
    bool kill = s.spriteY[s.playerBody] > 500 || PlayerTouches(s, s.lvBody[L11_KILLCEIL]) || PlayerTouches(s, s.lvBody[L11_KILLROTATE]);
    for (int k = 0; k < 3 && !kill; ++k) kill = PlayerTouches(s, s.lvBody[L11_STAR0 + k]);
    for (int k = 0; k < 4 && !kill; ++k) kill = PlayerTouches(s, s.lvBody[L11_BREVNO0 + k]);
    if (kill) s.PlayerDie();
}

static void NotImplemented(Sim&) { fatal("level not implemented yet"); }

// switchFrame[i] -> clip, per level (order used by the level scripts)
static const char* SwitchClip(int32_t level, int32_t i) {
    static const char* l5[] = {"blueCheck", "greenCheck"};
    static const char* l7[] = {"redCheck", "blueCheck"};
    static const char* l14[] = {"blueCheck"};
    static const char* l13[] = {"greenCheck"};
    static const char* l15[] = {"redCheck"};
    if (level == 5 && i < 2) return l5[i];
    if (level == 7 && i < 2) return l7[i];
    if (level == 14 && i < 1) return l14[i];
    if (level == 13 && i < 1) return l13[i];
    if (level == 15 && i < 1) return l15[i];
    return nullptr;
}
int32_t Sim::LoggedFlags() const {
    int32_t f = 0;
    for (int32_t i = 0; i < 3; ++i)
        if (staticFlag[i]) f |= 1 << i;
    for (int32_t i = 0; i < 4; ++i) {
        const char* c = SwitchClip(tpl->id, i);
        if (!c || switchFrame[i] == 1) continue;
        if (!std::strcmp(c, "blueCheck")) f |= 1 << 8;
        if (!std::strcmp(c, "greenCheck")) f |= 1 << 9;
        if (!std::strcmp(c, "redCheck")) f |= 1 << 10;
    }
    return f;
}

bool LevelVerified(int32_t id) {
    static const int32_t verified[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int32_t v : verified)
        if (v == id) return true;
    return false;
}
void WarnIfUnverified(int32_t id) {
    if (LevelVerified(id)) return;
    std::fprintf(stderr,
                 "warning: level %d is scripted from the AS3 but NOT verified against Flash logs yet; positions, spikes,\n"
                 "         switches and rotated bodies may differ from the game (README: Level status).\n",
                 id);
}

const LevelScript& GetLevelScript(int32_t id) {
    static const LevelScript scripts[] = {
        {1, L1_Construct, L1_Update, true},
        {2, L2_Construct, L2_Update, true},
        {3, L3_Construct, L3_Update, true},
        {4, L4_Construct, L4_Update, true},
        {5, L5_Construct, L5_Update, true, L5_Switches},
        {6, L6_Construct, L6_Update, true},
        {7, L7_Construct, L7_Update, true, L7_Switches},
        {12, L12_Construct, L12_Update, true},
        {14, L14_Construct, L14_Update, true, L14_Switches},
        {8, L8_Construct, L8_Update, true},
        {9, L9_Construct, L9_Update, true, L9_Switches},
        {10, L10_Construct, L10_Update, true},
        {11, L11_Construct, L11_Update, true},
        {13, L13_Construct, L13_Update, true, L13_Switches},
        {15, L15_Construct, L15_Update, true, L15_Switches},
        {17, L17_Construct, L17_Update, true},
        {16, L16_Construct, L16_Update, true, L16_Switches},
    };
    for (const LevelScript& ls : scripts)
        if (ls.id == id) return ls;
    static LevelScript missing{0, NotImplemented, NotImplemented, false};
    missing.id = id;
    return missing;
}

// ---------------------------------------------------------------- construction

void Sim::Load(LevelTemplate* t, int32_t checkpoint, bool keepStatics) {
    if (!keepStatics)
        for (bool& f : staticFlag) f = false;
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
    aimBody = -1;

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
    for (int32_t& f : switchFrame) f = 1;
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
    winMargin = 0;

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
double FlashTweenConstant() {
    const uint64_t bits = 0x3fe996a2ea68dd55ULL;
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

void Sim::CameraStep() {
    if (!camTween) return;
    // Math.pow(2, -10 * t / d) at t = 1, d = 31, as Flash computes it (E8pow calibration). A constant, so the
    // result never depends on the host's pow() (Windows and Linux libms may differ in the last bit).
    static const double p = [] {
        const uint64_t bits = 0x3fe996a2ea68dd55ULL;
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }();
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
    if (!o.exact) {  // rotated/scaled target: display_data.h holds an unrounded box (not calibrated, E10)
        const Rect in{r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1}, out{r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1};
        const Rect b = BallBounds(sx, sy, rw, 0);
        if (RectsHit(b, in) != RectsHit(b, out)) ++displayUncertain;
    }
    return RectsHit(BallBounds(sx, sy, rw, 0), r);
}

double Sim::TargetOverlap(const DisplayObj& o) const {
    const double ox = playerAlive ? 0 : camX * 20, oy = playerAlive ? 0 : camY * 20;
    const Rect b = BallBounds(spriteX[playerBody], spriteY[playerBody], spriteRotW[playerBody], 0);
    const double wx = std::fmin(b.x1, o.x1 + ox) - std::fmax(b.x0, o.x0 + ox);
    const double wy = std::fmin(b.y1, o.y1 + oy) - std::fmax(b.y0, o.y0 + oy);
    return std::fmin(wx, wy);
}

// levelAim's hit box. A static clip: the box of display_data.h. Level 11: the clip is the sprite of a dynamic body,
// so its box follows the sprite (x/y twip-quantised, rotation via FlashRotationMatrix). Flash's getBounds of a turned
// sprite transforms EACH CHILD's box separately and unions the results (not the rotated union box): the levelAim
// symbol (68) holds the pole (shape 65) and the flag cloth (morph 64 at ratio 0, startBounds). Fitted on the Level 11
// logs: 11 near-contact frames with the flag turned -4.3..49 degrees, all agree; the rotated-union model got 10 wrong.
// Corner rounding (16.16 product -> twips) is taken as round-to-nearest like the ball's box; the logs cannot tell
// it from floor/truncation, so a decision those modes would flip counts in displayUncertain.
static const int64_t kAimChildren[2][4] = {{-20, -184, 106, 626}, {78, 71, 282, 255}};  // twips, clip space
static int64_t AimRound(int64_t v, int mode) {
    if (mode == 0) return (v + 32768) >> 16;
    if (mode == 1) return v >> 16;
    return v >= 0 ? v >> 16 : -((-v) >> 16);
}
static DisplayObj AimBox(const DisplayObj& base, int64_t tx, int64_t ty, const FlashMatrix& m, int mode) {
    DisplayObj o = base;
    bool first = true;
    for (const auto& r : kAimChildren) {
        const int64_t px[4] = {r[0], r[2], r[0], r[2]}, py[4] = {r[1], r[1], r[3], r[3]};
        for (int i = 0; i < 4; ++i) {
            const double x = (double)(tx + AimRound((int64_t)m.a * px[i] - (int64_t)m.b * py[i], mode));
            const double y = (double)(ty + AimRound((int64_t)m.b * px[i] + (int64_t)m.a * py[i], mode));
            if (first) o.x0 = o.x1 = x, o.y0 = o.y1 = y, first = false;
            o.x0 = std::fmin(o.x0, x), o.x1 = std::fmax(o.x1, x), o.y0 = std::fmin(o.y0, y), o.y1 = std::fmax(o.y1, y);
        }
    }
    return o;
}
DisplayObj Sim::GoalTarget() {
    DisplayObj o = *tpl->aim;
    if (aimBody < 0) return o;
    const RawPlacement& p = tpl->Place("levelAim");
    if (p.a != 65536 || p.b != 0 || p.c != 0 || p.d != 65536) fatal("moving levelAim with a transformed placement");
    if (o.x0 - p.tx != -20 || o.y0 - p.ty != -184 || o.x1 - p.tx != 282 || o.y1 - p.ty != 626)
        fatal("levelAim symbol bounds differ from kAimChildren");
    const int64_t tx = std::llround(spriteX[aimBody] * 20), ty = std::llround(spriteY[aimBody] * 20);
    const FlashMatrix m = FlashRotationMatrix(spriteRotW[aimBody]);
    o = AimBox(o, tx, ty, m, 0);
    if (m.b != 0 || m.a != 65536) {
        const Rect ball = BallBounds(spriteX[playerBody], spriteY[playerBody], spriteRotW[playerBody], 0);
        const double ox = playerAlive ? 0 : camX * 20, oy = playerAlive ? 0 : camY * 20;
        bool h[3];
        for (int mode = 0; mode < 3; ++mode) {
            const DisplayObj c = AimBox(o, tx, ty, m, mode);
            h[mode] = RectsHit(ball, Rect{c.x0 + ox, c.y0 + oy, c.x1 + ox, c.y1 + oy});
        }
        if (h[0] != h[1] || h[0] != h[2]) ++displayUncertain;
    }
    return o;
}

// Level.Update after the camera tween: levelAim test, spikes, checkpoints (in this order).
void Sim::DisplayUpdate() {
    if (tpl->aim && aimFrame == 1) {  // no IsLive() guard
        const DisplayObj goal = GoalTarget();
        if (BallHitsTarget(goal)) {
            winMargin = TargetOverlap(goal);
            PlayerWin();
        }
    }
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

void Sim::Tick(uint8_t input) {
    if (isTimeStop) return;  // Game.tPause == 0 after PlayerWin: no more Level.Update calls
    // Game.UpdateHandler (playback): Left = v>=4, Up = v>=6||v==2||v==3, Right = v%2==1
    bool left = input >= 4;
    bool up = input >= 6 || input == 2 || input == 3;
    bool right = input % 2 == 1;
    // After a death Flash keeps calling the full Level.Update until R: the camera eases toward the frozen ball, the
    // world steps (without the ball, whose body was destroyed; the random debris is not simulated), the level's
    // machinery and Level_N.Update run, and the goal/checkpoint/switch tests use the dead-ball rule (death warp).
    // The key handlers still feed the destroyed ball body (velocity/forces on a body outside the world: no effect).
    const bool dead = !playerAlive;
    LevelUpdate(left, up, right);
    if (dead) ++deadTicks;
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
