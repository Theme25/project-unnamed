// verify.cpp - replay rb1_stats.tsv logs (see docs/STATS_LOGGING.md) and
// compare every field bit-for-bit against the simulator.
#include "redball.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace rb;

namespace {

uint64_t Bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}
double FromHex(const std::string& h) {
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

struct Entry {
    bool restart = false;
    std::vector<std::string> f;  // 21 columns
    int line = 0;
};
struct Segment {
    int line = 0;
    std::vector<Entry> entries;  // entries[0] = tick-0 state line
};

const char* kField[] = {"tick", "frame", "in", "px", "py", "vx", "vy", "a",  "w",  "sleepT", "flags",
                        "pC",   "pL",    "pR", "nCB", "cb", "nC", "sx", "sy", "sr", "ts"};

// Returns the list of mismatching column indices.
std::vector<int> Compare(const FrameStats& s, const std::vector<std::string>& f, std::string& detail) {
    std::vector<int> bad;
    auto cmpD = [&](int col, double mine) {
        if (Bits(FromHex(f[(size_t)col])) != Bits(mine)) {
            bad.push_back(col);
            char buf[160];
            std::snprintf(buf, sizeof buf, "  %-6s flash=%.17g  sim=%.17g\n", kField[col], FromHex(f[(size_t)col]), mine);
            detail += buf;
        }
    };
    auto cmpI = [&](int col, long long mine) {
        if (std::stoll(f[(size_t)col]) != mine) {
            bad.push_back(col);
            detail += "  " + std::string(kField[col]) + " flash=" + f[(size_t)col] + "  sim=" + std::to_string(mine) + "\n";
        }
    };
    cmpD(3, s.px);
    cmpD(4, s.py);
    cmpD(5, s.vx);
    cmpD(6, s.vy);
    cmpD(7, s.angle);
    cmpD(8, s.omega);
    cmpD(9, s.sleepTime);
    cmpI(10, s.flags);
    cmpI(11, s.probeCenter);
    cmpI(12, s.probeLeft);
    cmpI(13, s.probeRight);
    cmpI(14, s.contactCount);
    if (f[15] != s.contactNames) {
        bad.push_back(15);
        detail += "  cb     flash=[" + f[15] + "]  sim=[" + s.contactNames + "]\n";
    }
    cmpI(16, s.worldContactCount);
    cmpD(17, s.sx);
    cmpD(18, s.sy);
    cmpD(19, s.sr);
    return bad;
}

}  // namespace

int CmdVerify(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: rbsim verify <rb1_stats.tsv> [--verbose N] [--ignore sr,...]\n");
        return 2;
    }
    int verbose = 5;
    std::vector<bool> ignore(21, false);
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--verbose") && i + 1 < argc) verbose = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ignore") && i + 1 < argc) {
            std::string list = argv[++i];
            for (int c = 0; c < 21; ++c)
                if (list.find(kField[c]) != std::string::npos) ignore[(size_t)c] = true;
        }
    }
    std::ifstream in(argv[2]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    std::vector<Segment> segs;
    std::string line;
    int ln = 0, skipped = 0;
    while (std::getline(in, line)) {
        ++ln;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("LEVEL", 0) == 0) {
            if (line != "LEVEL 1 0") {
                std::fprintf(stderr, "line %d: only 'LEVEL 1 0' segments are supported yet (%s)\n", ln, line.c_str());
                segs.push_back(Segment());
                segs.back().line = -ln;  // marks unsupported
                continue;
            }
            segs.push_back(Segment());
            segs.back().line = ln;
            continue;
        }
        if (segs.empty() || segs.back().line < 0) {
            ++skipped;
            continue;
        }
        Entry e;
        e.line = ln;
        if (line == "R") e.restart = true;
        else {
            e.f = Split(line);
            if (e.f.size() < 21) {
                std::fprintf(stderr, "line %d: expected 21 columns, got %zu\n", ln, e.f.size());
                continue;
            }
        }
        segs.back().entries.push_back(e);
    }

    LevelTemplate tpl(MakeLevel1());
    auto sim = std::make_unique<Sim>();
    int perfect = 0, diverged = 0, unsupported = 0, shown = 0;
    long framesCompared = 0, framesMatched = 0;
    std::map<std::string, int> firstFieldHist;
    std::map<int, int> divergeTickHist;
    for (size_t si = 0; si < segs.size(); ++si) {
        Segment& sg = segs[si];
        if (sg.line < 0 || sg.entries.empty()) {
            ++unsupported;
            continue;
        }
        sim->Load(&tpl);
        bool ok = true;
        int endReason = 0;  // 0 = end of log, 1 = death, 2 = win
        for (size_t ei = 0; ei < sg.entries.size(); ++ei) {
            const Entry& e = sg.entries[ei];
            if (e.restart) {
                sim->Restart();
                continue;
            }
            uint8_t code = (uint8_t)std::stoi(e.f[2]);
            if (ei > 0) sim->Tick(code);
            FrameStats st = sim->Stats(code);
            std::string detail;
            std::vector<int> bad = Compare(st, e.f, detail);
            std::vector<int> relevant;
            for (int c : bad)
                if (!ignore[(size_t)c]) relevant.push_back(c);
            ++framesCompared;
            if (relevant.empty()) {
                ++framesMatched;
            } else {
                ok = false;
                std::string key;
                for (int c : relevant) key += std::string(key.empty() ? "" : "+") + kField[c];
                firstFieldHist[key]++;
                divergeTickHist[std::stoi(e.f[0]) / 100 * 100]++;
                if (shown < verbose) {
                    ++shown;
                    std::printf("segment %zu (log line %d): first divergence at tick %s (log line %d), input %d\n", si,
                                sg.line, e.f[0].c_str(), e.line, code);
                    std::printf("%s", detail.c_str());
                    // context: previous inputs
                    std::string hist;
                    for (size_t k = (ei > 12 ? ei - 12 : 1); k <= ei; ++k)
                        if (!sg.entries[k].restart) hist += sg.entries[k].f[2];
                    std::printf("  inputs leading up: %s\n\n", hist.c_str());
                }
                break;
            }
            if (e.f[20] == "1") {
                endReason = 2;
                break;
            }
            if (!sim->playerAlive) {
                endReason = 1;
                break;
            }
        }
        (void)endReason;
        if (ok) ++perfect;
        else ++diverged;
    }
    std::printf("==== summary ====\n");
    std::printf("segments: %zu  (bit-exact to end/death/win: %d, diverged: %d, unsupported: %d)\n", segs.size(),
                perfect, diverged, unsupported);
    std::printf("frames compared: %ld, matched: %ld  (log lines before first LEVEL skipped: %d)\n", framesCompared,
                framesMatched, skipped);
    if (!firstFieldHist.empty()) {
        std::printf("first-divergence fields:\n");
        for (auto& kv : firstFieldHist) std::printf("  %-28s %d\n", kv.first.c_str(), kv.second);
    }
    return diverged ? 1 : 0;
}
