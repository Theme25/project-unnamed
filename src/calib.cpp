// calib.cpp - `rbsim calib <rb1_calib.tsv>`: checks the display-layer model against the
// E1 / E2 / E3 rows of a Flash calibration dump (docs/STATS_LOGGING.md 3.5). Level 2 geometry.
#include "redball.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
        } else if (f[0] == "E4" && f.size() >= 6) {
            checkM(H(f[2]), f[3], f[4]);
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
    std::printf("E2 hitTestObject sweep: %ld rows; RectsHit on Flash's own bounds agrees in %ld (touching-edge rows: %ld)\n",
                e2, e2obs, e2touch);
    std::printf("   with the modelled ball box: agrees in %ld, consistent with some +-1 twip adjustment in %ld\n", e2model,
                e2inRange);
    std::printf("E3 static bounds vs SWF-derived display_data.h: %ld / %ld identical\n", e3ok, e3n);
    const bool pass = e1far == 0 && e2obs == e2 && e2inRange == e2 && e1exact == e1 && (mN == 0 || mN - mOk <= 4);
    std::printf("%s\n", pass ? "CALIB OK" : "CALIB MISMATCH");
    return pass ? 0 : 1;
}
