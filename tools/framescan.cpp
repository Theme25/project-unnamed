// framescan LOG [--level N] [--first FS]
// Checks the per-frame edge scans ("#FS" rows) that the frame-scan Shipik mod (docs/resolution/Shipik_framescan.as)
// writes under MATHSPIKES 0, against rbsim's model of Flash's shape test (src/flashpix.h): for every scanned row of
// every spike near the ball, the hit/miss transitions along the row must equal the model's with the spike's edges
// as of the redraw d frames earlier (d = 1 is the model; d = 0 would mean no edge cache).
//
//   #FS <n> <Level.x> <Level.y> <ball x> <ball y> {<i>.<j>@<y>[<left>,<top>],<x>:<0|1>,...}...
//       n: frame counter of the mod (EXIT_FRAME), Level/ball positions as hex doubles (px), then per scanned row:
//       the spike (Level child i, Ships10 child j), the row's stage twip y, the spike's stage bounds (twips) and
//       the stage twips x where hitTestPoint(x+0.5, y+0.5, true) changes.
//   #FSLEVEL <n>: a Level was built at frame counter n (the rows of a playback start after the last one).
//
// Build: make framescan
#include "../src/flashpix.h"
#include "../src/redball.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace rb;

static double Hex(const std::string& h) {
    const uint64_t u = std::strtoull(h.c_str(), nullptr, 16);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

static pix::DevMatrix Dev(const pix::Window& w, const SpikeObj& s, int64_t LX, int64_t LY) {
    pix::DevMatrix m = pix::Concat(pix::RootMatrix(w), 1, 0, 0, 1, LX, LY);
    for (int i = s.n - 1; i >= 0; --i) {
        const SpikeMat& c = s.chain[i];
        m = pix::Concat(m, c.a, c.b, c.c, c.d, std::llround(c.tx), std::llround(c.ty));
    }
    return m;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: framescan LOG [--level N] [--first FS]\n");
        return 2;
    }
    int level = 3, first = -1;
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--level")) level = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--first")) first = std::atoi(argv[++i]);
    }
    int W = 0, H = 0, lastFsLevel = -1;
    bool showAll = true;
    std::ifstream in(argv[1]);
    std::string line;
    std::vector<std::vector<std::string>> rows;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string x;
        while (std::getline(ss, x, '\t')) f.push_back(x);
        if (f.empty()) continue;
        if (f[0] == "#ENV" && !W)
            for (size_t i = 1; i + 1 < f.size(); i += 2) {
                if (f[i] == "window") std::sscanf(f[i + 1].c_str(), "%dx%d", &W, &H);
                if (f[i] == "scaleMode") showAll = f[i + 1] != "noScale";
            }
        if (f[0] == "#FSLEVEL") lastFsLevel = std::atoi(f[1].c_str());
        if (f[0] == "#FS") rows.push_back(f);
    }
    if (!W) {
        std::fprintf(stderr, "no #ENV line with the window size\n");
        return 2;
    }
    if (first < 0) first = lastFsLevel;
    const pix::Window w = pix::MakeWindow(W, H, showAll);
    LevelTemplate t(level);
    std::map<int, std::pair<int64_t, int64_t>> lev;  // frame counter -> Level position (twips)
    for (auto& f : rows) lev[std::atoi(f[1].c_str())] = {std::llround(Hex(f[2]) * 20), std::llround(Hex(f[3]) * 20)};
    std::printf("window %dx%d (%s), level %d, frames from %d\n", W, H, showAll ? "showAll" : "noScale", level, first);
    long total = 0, match[4] = {0, 0, 0, 0};
    for (auto& f : rows) {
        const int n = std::atoi(f[1].c_str());
        if (n < first || f.size() < 7) continue;
        const auto L = lev[n];
        for (size_t c = 6; c < f.size(); ++c) {
            const std::string& s = f[c];
            const size_t at = s.find('@');
            int y, left, top;
            if (at == std::string::npos || std::sscanf(s.c_str() + at + 1, "%d[%d,%d]", &y, &left, &top) != 3) continue;
            std::vector<std::pair<int, int>> flash;
            for (size_t p = s.find(']') + 1; p < s.size() && s[p] == ',';) {
                int xx, v;
                std::sscanf(s.c_str() + p + 1, "%d:%d", &xx, &v);
                flash.push_back({xx, v});
                p = s.find(',', p + 1);
                if (p == std::string::npos) break;
            }
            int best = -1;
            long bd = 1L << 60;
            for (int i = 0; i < t.spikeCount; ++i) {
                const long d = labs((long)(L.first + t.spikes[i].bx0) - left) + labs((long)(L.second + t.spikes[i].by0) - top);
                if (d < bd) bd = d, best = i;
            }
            if (bd > 4) {
                std::printf("frame %d: no spike at stage (%d,%d)\n", n - first, left, top);
                continue;
            }
            ++total;
            const pix::DevMatrix now = Dev(w, t.spikes[best], L.first, L.second);
            for (int d = 0; d < 4; ++d) {
                if (!lev.count(n - d) || n - d < first) continue;
                const pix::DevMatrix drawn = Dev(w, t.spikes[best], lev[n - d].first, lev[n - d].second);
                std::vector<std::pair<int, int>> model;
                bool st = false;
                for (int x = left - 400; x <= left + 520; ++x) {
                    int64_t px, py;
                    pix::Probe(w, x, y, px, py);
                    const bool h = pix::SpikeShapeHit(now, drawn, px, py);
                    if (h != st) model.push_back({x, h}), st = h;
                }
                if (model == flash) ++match[d];
                else if (d == 1)
                    std::printf("frame %d spike %d row y=%d: Flash %zu transitions%s, model %zu%s\n", n - first, best, y, flash.size(),
                                flash.empty() ? "" : (" from x=" + std::to_string(flash[0].first)).c_str(), model.size(),
                                model.empty() ? "" : (" from x=" + std::to_string(model[0].first)).c_str());
            }
        }
    }
    std::printf("scanned rows: %ld; reproduced with the edges of the redraw d frames back: d=0 %ld, d=1 %ld (the model), d=2 %ld, d=3 %ld\n",
                total, match[0], match[1], match[2], match[3]);
    return match[1] == total ? 0 : 1;
}
