// calib.cpp - `rbsim calib <rb1_calib.tsv>`: checks the display-layer model against the
// E1 / E2 / E3 rows of a Flash calibration dump (docs/STATS_LOGGING.md 3.5). Level 2 geometry.
#include "redball.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace rb;

namespace {
double H(const std::string& h) {
    uint64_t u = std::stoull(h, nullptr, 16);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find('\t', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}
long Tw(double px) { return std::lround(px * 20); }
double SpriteCoordC(double v) { return (double)as3_toInt32(v * 20) / 20.0; }
}  // namespace

int CmdCalib(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: rbsim calib <rb1_calib.tsv>\n");
        return 2;
    }
    std::ifstream in(argv[2]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    LevelTemplate tpl(2);
    const DisplayObj* aim = tpl.aim;
    long e1 = 0, e1exact = 0, e1plus = 0, e1minus = 0, e1far = 0, e1sym = 0;
    long e2 = 0, e2obs = 0, e2model = 0, e2inRange = 0, e2touch = 0;
    long e3ok = 0, e3n = 0;
    long mN = 0, mOk = 0;  // E4 / E7a / E7b: rotation -> matrix
    long e8cN = 0, e8cOk = 0, e8cHits = 0, powN = 0, powOk = 0;
    long e9N = 0, e9Ok = 0, e9RotN = 0, e9RotOk = 0, e9bN = 0, e9bOk = 0, e10N = 0, e10Ok = 0, e10Approx = 0, e10Other = 0;
    long e10Targets = 0, e9Other = 0;
    std::set<std::string> bodyNames;
    int bodyNamesLevel = -1;
    int dumpLevel = 8;  // "E9level <id>" row (docs/STATS_LOGGING.md 3.0); older dumps were Level 8 only
    std::unique_ptr<LevelTemplate> tplL;
    auto lvl = [&]() -> LevelTemplate& {
        if (!tplL || tplL->id != dumpLevel) tplL.reset(new LevelTemplate(dumpLevel));
        return *tplL;
    };
    LevelTemplate tpl3(3);
    auto checkM = [&](double rot, const std::string& ha, const std::string& hb) {
        const FlashMatrix m = FlashRotationMatrix(rot);
        ++mN;
        if (m.a == std::lround(H(ha) * 65536) && m.b == std::lround(H(hb) * 65536)) ++mOk;
    };
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = Split(line);
        if ((f[0] == "E1" || f[0] == "E1a") && f.size() >= 9) {
            const double x = H(f[2]), y = H(f[3]), rot = H(f[4]);
            const long cx = Tw(x), cy = Tw(y);
            const long l = Tw(H(f[5])), t = Tw(H(f[6])), r = Tw(H(f[5]) + H(f[7])), b = Tw(H(f[6]) + H(f[8]));
            ++e1;
            if (cx - l == r - cx && cy - t == b - cy && cx - l == cy - t) ++e1sym;
            const Rect m = BallBounds(x, y, rot, 0);
            const long d = (cx - l) - std::lround(cx - m.x0);  // 0 when the model is exact
            if (d == 0) ++e1exact;
            else if (d == 1) ++e1plus;
            else if (d == -1) ++e1minus;
            else ++e1far;
        } else if (f[0] == "E2" && f.size() >= 9 && aim) {
            const double x = H(f[1]), y = H(f[2]), rot = H(f[3]);
            const int hit = std::stoi(f[4]);
            const Rect obs{(double)Tw(H(f[5])), (double)Tw(H(f[6])), (double)Tw(H(f[5]) + H(f[7])),
                           (double)Tw(H(f[6]) + H(f[8]))};
            const Rect g{aim->x0, aim->y0, aim->x1, aim->y1};
            ++e2;
            if (RectsHit(obs, g) == (hit == 1)) ++e2obs;
            if (RectsHit(BallBounds(x, y, rot, 0), g) == (hit == 1)) ++e2model;
            const bool lo = RectsHit(BallBounds(x, y, rot, -1), g), hi = RectsHit(BallBounds(x, y, rot, +1), g);
            if ((lo && hit == 1) || (!lo && hi) || (!hi && hit == 0)) ++e2inRange;  // consistent with some adj in [-1,1]
            const double ox = std::fmin(obs.x1, g.x1) - std::fmax(obs.x0, g.x0);
            const double oy = std::fmin(obs.y1, g.y1) - std::fmax(obs.y0, g.y0);
            if ((ox == 0 && oy >= 0) || (oy == 0 && ox >= 0)) ++e2touch;
        } else if (f[0] == "E8pow" && f.size() >= 2) {
            ++powN;
            if (H(f[1]) == std::pow(2.0, -10 * 1.0 / 31)) ++powOk;
        } else if (f[0] == "E8c" && f.size() >= 5) {
            // pb.HitTestObjectControlPoints(L.ship1) with the covers at (0,0): ship1 = first 11 Level 3 spikes
            const double rot = H(f[1]), x = SpriteCoordC(H(f[2])), y = SpriteCoordC(H(f[3]));
            bool hit = false;
            for (int32_t i = 0; i < 11 && i < tpl3.spikeCount; ++i)
                if (BallHitsSpike(x, y, rot, 0, 0, tpl3.spikes[i]).hit) hit = true;
            ++e8cN;
            if (hit == (std::stoi(f[4]) == 1)) ++e8cOk;
            if (std::stoi(f[4]) == 1) ++e8cHits;
        } else if (f[0] == "E9level" && f.size() >= 2) {
            dumpLevel = std::stoi(f[1]);
        } else if (f[0] == "E10" && f.size() >= 6) {
            // getBounds(L) of a named first-frame object vs display_data.h
            const DisplayObj* o = lvl().Display(f[1].c_str());
            if (!o) continue;
            ++e10N;
            const long x0 = Tw(H(f[2])), y0 = Tw(H(f[3])), x1 = Tw(H(f[2]) + H(f[4])), y1 = Tw(H(f[3]) + H(f[5]));
            // only goals, checkpoints and switch clips are hit-tested by the sim (spikes have their own data)
            const std::string& nm = f[1];
            const bool target = nm == "levelAim" || nm.rfind("checkPoint", 0) == 0 || nm == "blueCheck" || nm == "greenCheck" ||
                                nm == "redCheck";
            if (target) ++e10Targets;
            if (x0 == std::lround(o->x0) && y0 == std::lround(o->y0) && x1 == std::lround(o->x1) && y1 == std::lround(o->y1)) ++e10Ok;
            else if (!o->exact) ++e10Approx;  // rotated/scaled: display_data.h holds an approximation
            else if (!target) {
                ++e10Other;
                std::printf("  E10 L%d %-14s (not hit-tested) flash=[%ld %ld %ld %ld]  generated=[%.0f %.0f %.0f %.0f]\n", dumpLevel,
                            f[1].c_str(), x0, y0, x1, y1, o->x0, o->y0, o->x1, o->y1);
            } else std::printf("  E10 L%d %-14s flash=[%ld %ld %ld %ld]  swf=[%.0f %.0f %.0f %.0f]\n", dumpLevel, f[1].c_str(), x0, y0, x1, y1,
                             o->x0, o->y0, o->x1, o->y1);
        } else if (f[0] == "E9a" && f.size() >= 11) {
            // placements: position + 16.16 matrix vs levels_data.h; rotated body clips vs the measured table
            LevelTemplate& tpl8 = lvl();
            if (bodyNamesLevel != dumpLevel) {  // which clips become physics bodies on this level
                bodyNames.clear();
                auto sim = std::make_unique<Sim>();
                sim->Load(&tpl8);
                for (int32_t b = 0; b < sim->world.bodyCount; ++b)
                    if (sim->world.bodies[b].inWorld) bodyNames.insert(sim->BodyName(b));
                bodyNamesLevel = dumpLevel;
            }
            bool known = tpl8.HasPlacement(f[1].c_str());
            if (!known) continue;  // Shipik covers/masks and unnamed instances
            const RawPlacement& p = tpl8.Place(f[1].c_str());
            ++e9N;
            // duplicate names (shipik7): accept either placement by checking position against this row
            const bool posOk = std::lround(H(f[2]) * 20) == p.tx && std::lround(H(f[3]) * 20) == p.ty;
            const bool matOk = std::lround(H(f[7]) * 65536) == p.a && std::lround(H(f[8]) * 65536) == p.b &&
                               std::lround(H(f[9]) * 65536) == p.c && std::lround(H(f[10]) * 65536) == p.d;
            const bool used = bodyNames.count(f[1]) > 0;
            if ((posOk && matOk) || f[1] == "shipik7") ++e9Ok;
            else if (!used) {
                ++e9Other;  // e.g. TextField x/y include the text box margin; not a body, not used by the sim
                std::printf("  E9a %s differs from levels_data.h (not a physics body: ignored)\n", f[1].c_str());
            } else std::printf("  E9a %s differs from levels_data.h\n", f[1].c_str());
            if (!(p.b == 0 && p.c == 0 && p.a > 0) && std::strncmp(f[1].c_str(), "shipik", 6)) {
                double v = 0;
                ++e9RotN;
                if (LookupTimelineRotation(dumpLevel, f[1].c_str(), v) && v == H(f[4])) ++e9RotOk;
                else std::printf("  E9a %s rotation %.17g not in the measured table\n", f[1].c_str(), H(f[4]));
            }
        } else if (f[0] == "E9b" && f.size() >= 9) {
            // rotation getter of a code-set matrix: atan2(b, a) * 180 / PI on the stored entries
            ++e9bN;
            const double r = std::atan2(H(f[3]), H(f[2])) * 180 / AS3_PI;
            if (std::fabs(r - H(f[6])) <= 1e-12) ++e9bOk;
        } else if (f[0] == "E4" && f.size() >= 6) {
            checkM(-180 + std::stoi(f[1]) * 0.7317, f[3], f[4]);  // value written (3.6), not the read-back
        } else if (f[0] == "E7a" && f.size() >= 8) {
            checkM(H(f[3]), f[4], f[5]);
        } else if (f[0] == "E7b" && f.size() >= 7) {
            checkM(H(f[2]), f[3], f[4]);
        } else if (f[0] == "E3" && f.size() >= 10) {
            const DisplayObj* o = tpl.Display(f[1].c_str());
            if (!o) continue;
            ++e3n;
            const long bx = Tw(H(f[2])), by = Tw(H(f[3])), bx1 = Tw(H(f[2]) + H(f[4])), by1 = Tw(H(f[3]) + H(f[5]));
            const bool same = bx == std::lround(o->x0) && by == std::lround(o->y0) && bx1 == std::lround(o->x1) &&
                              by1 == std::lround(o->y1);
            if (same) ++e3ok;
            else
                std::printf("  E3 %-13s flash=[%ld %ld %ld %ld]  swf=[%.2f %.2f %.2f %.2f]\n", f[1].c_str(), bx, by, bx1, by1,
                            o->x0, o->y0, o->x1, o->y1);
        }
    }
    std::printf("E1 rotated ball bounds: %ld rows, symmetric integral half-extent in %ld\n", e1, e1sym);
    std::printf("   Flash-matrix model: exact %ld, flash +1 twip %ld, flash -1 twip %ld, worse %ld\n", e1exact, e1plus, e1minus,
                e1far);
    if (mN) std::printf("rotation -> 16.16 matrix (E4/E7a/E7b): %ld / %ld exact\n", mOk, mN);
    if (e9N)
        std::printf("Level %d placements (E9a): %ld / %ld match; rotated body clips with a measured rotation: %ld / %ld\n", dumpLevel,
                    e9Ok, e9N, e9RotOk, e9RotN);
    if (e10N)
        std::printf("Level %d object bounds (E10): %ld / %ld exact (hit-test targets: %ld, all must match); %ld other objects differ, %ld "
                    "rotated/scaled approximate\n",
                    dumpLevel, e10Ok, e10N, e10Targets, e10Other, e10Approx);
    if (e9bN) std::printf("rotation getter of code-set matrices, atan2(b,a)*180/PI within 1e-12 deg (E9b): %ld / %ld\n", e9bOk, e9bN);
    if (powN) std::printf("camera tween constant Math.pow(2, -10/31): %s\n", powOk == powN ? "identical" : "DIFFERENT");
    if (e8cN) std::printf("standardized spikes, whole check (E8c): %ld / %ld (Flash hits: %ld)\n", e8cOk, e8cN, e8cHits);
    std::printf("E2 hitTestObject sweep: %ld rows; RectsHit on Flash's own bounds agrees in %ld (touching-edge rows: %ld)\n",
                e2, e2obs, e2touch);
    std::printf("   with the modelled ball box: agrees in %ld, consistent with some +-1 twip adjustment in %ld\n", e2model,
                e2inRange);
    std::printf("E3 static bounds vs SWF-derived display_data.h: %ld / %ld identical\n", e3ok, e3n);
    const bool e8ok = e8cOk == e8cN && powOk == powN && e9Ok + e9Other == e9N && e9RotOk == e9RotN && e9bOk == e9bN && e10Ok + e10Approx + e10Other == e10N;
    const bool pass = e8ok && e1far == 0 && e2obs == e2 && e2inRange == e2 && e1exact == e1 && (mN == mOk);
    std::printf("%s\n", pass ? "CALIB OK" : "CALIB MISMATCH");
    return pass ? 0 : 1;
}
