// verify.cpp - replay rb1_stats.tsv logs (see docs/STATS_LOGGING.md) and
// compare every field bit-for-bit against the simulator.
#include "redball.h"
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
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
    int level = 1, checkpoint = 0;
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
    cmpI(20, s.timeStop);
    return bad;
}

// Optional extension: after the 21 standard columns, groups of 7 columns
// <name> <px> <py> <a> <vx> <vy> <w> (hex) for other dynamic bodies.
// Returns false on mismatch (appending a description to `detail`).
static long g_cameraFramesChecked = 0, g_matrixFramesChecked = 0;
bool CompareExtraBodies(const Sim& sim, const std::vector<std::string>& f, std::string& detail, int& groups) {
    groups = 0;
    bool ok = true;
    for (size_t i = 21; i + 7 <= f.size(); i += 7) {
        ++groups;
        const std::string& name = f[i];
        if (name.rfind("playerDiePart", 0) == 0) continue;  // random death debris: not simulated
        int32_t body = -1;
        for (int32_t b = 0; b < sim.world.numBodies; ++b)
            if (sim.world.bodies[b].inWorld && sim.BodyName(b) == name) body = b;
        if (body < 0) {
            detail += "  body '" + name + "' logged by Flash but missing in sim\n";
            ok = false;
            continue;
        }
        const Body& B = sim.world.bodies[body];
        const double mine[6] = {B.xf.position.x, B.xf.position.y, B.sweep.a,
                                B.linearVelocity.x, B.linearVelocity.y, B.angularVelocity};
        static const char* fld[6] = {"px", "py", "a", "vx", "vy", "w"};
        for (int k = 0; k < 6; ++k) {
            double flash = FromHex(f[i + 1 + (size_t)k]);
            if (Bits(flash) != Bits(mine[k])) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "  %s.%-3s flash=%.17g  sim=%.17g\n", name.c_str(), fld[k], flash, mine[k]);
                detail += buf;
                ok = false;
            }
        }
    }
    // Trailing block (docs/STATS_LOGGING.md 3.8): ball matrix a b, Level.x Level.y dp[0] dp[1]
    if (f.size() >= 27 && (f.size() - 21) % 7 == 6) {
        const size_t c = f.size() - 6;
        if (sim.playerAlive) {
            const FlashMatrix m = FlashRotationMatrix(sim.spriteRotW[sim.playerBody]);
            const double fa = FromHex(f[c]), fb = FromHex(f[c + 1]);
            if (std::lround(fa * 65536) != m.a || std::lround(fb * 65536) != m.b) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "  ball matrix flash=(%ld,%ld)  sim=(%d,%d) /65536\n", std::lround(fa * 65536),
                              std::lround(fb * 65536), m.a, m.b);
                detail += buf;
                ok = false;
            }
            ++g_matrixFramesChecked;
        }
        const double mine[4] = {sim.camX, sim.camY, sim.dpX, sim.dpY};
        static const char* fld[4] = {"Level.x", "Level.y", "dp[0]", "dp[1]"};
        for (int k = 0; k < 4; ++k) {
            const double flash = FromHex(f[c + 2 + (size_t)k]);
            if (std::isnan(flash)) continue;  // dp before the first Update
            if (Bits(flash) != Bits(mine[k])) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "  %-7s flash=%.17g  sim=%.17g\n", fld[k], flash, mine[k]);
                detail += buf;
                ok = false;
            }
        }
        ++g_cameraFramesChecked;
    }
    return ok;
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
            int lv = 0, cp = 0;
            segs.push_back(Segment());
            if (std::sscanf(line.c_str(), "LEVEL %d %d", &lv, &cp) != 2 || !GetLevelScript(lv).implemented) {
                std::fprintf(stderr, "line %d: level not supported yet (%s)\n", ln, line.c_str());
                segs.back().line = -ln;  // marks unsupported
                continue;
            }
            segs.back().line = ln;
            segs.back().level = lv;
            segs.back().checkpoint = cp;
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

    std::map<int, std::unique_ptr<LevelTemplate>> tpls;
    auto sim = std::make_unique<Sim>();
    int perfect = 0, diverged = 0, unsupported = 0, shown = 0, extraGroupsSeen = 0, deathsMatched = 0;
    long framesCompared = 0, framesMatched = 0, trailingSkipped = 0, deathInputFromPrev = 0, postDeathFrames = 0, winsAfterDeath = 0;
    std::map<std::string, int> firstFieldHist;
    std::map<int, int> divergeTickHist;
    for (size_t si = 0; si < segs.size(); ++si) {
        Segment& sg = segs[si];
        if (sg.line < 0 || sg.entries.empty()) {
            ++unsupported;
            continue;
        }
        auto& tp = tpls[sg.level];
        if (!tp) tp.reset(new LevelTemplate(sg.level));
        sim->Load(tp.get(), sg.checkpoint);
        bool ok = true;
        int endReason = 0;  // 0 = end of log, 1 = death, 2 = win
        bool diedCounted = false;
        long segFrameBase = 0;
        for (const Entry& e0 : sg.entries)
            if (!e0.restart) {
                segFrameBase = std::stol(e0.f[1]);
                break;
            }
        for (size_t ei = 0; ei < sg.entries.size(); ++ei) {
            const Entry& e = sg.entries[ei];
            if (e.restart) {
                diedCounted = false;
                // lastCheckNum is static in AS3: it decides which checkpoint the next segment starts at
                if (si + 1 < segs.size() && segs[si + 1].level == sg.level && segs[si + 1].checkpoint != sim->lastCheckNum) {
                    std::printf("segment %zu: checkpoint carried into the restart differs: flash=%d sim=%d\n", si,
                                segs[si + 1].checkpoint, sim->lastCheckNum);
                    ok = false;
                }
                sim->Restart();
                continue;
            }
            // Artifact: the last row of a log can be written without the frame counter advancing
            // (the stop/export keypress triggers one more Update), and its "in" field does not
            // reflect the keys the game actually read. Skip such a trailing row.
            // (the win row also repeats the previous frame number, frameCount stops at the win: compare it)
            const bool winRow = e.f[20] == "1" && ei > 0 && !sg.entries[ei - 1].restart && sg.entries[ei - 1].f[20] != "1";
            if (!winRow && ei > 0 && !sg.entries[ei - 1].restart && e.f[1] == sg.entries[ei - 1].f[1]) {
                bool tail = true;
                for (size_t k = ei; k < sg.entries.size(); ++k)
                    if (sg.entries[k].restart || sg.entries[k].f[1] != e.f[1]) tail = false;
                if (tail) {
                    trailingSkipped += (long)(sg.entries.size() - ei);
                    break;
                }
            }
            // After the death frame: the ball's body is gone and the world holds random debris, so only
            // what the sim models after death is compared: frame counter, win flag, camera and dp.
            if (ei > 0 && !sim->playerAlive) {
                sim->Tick((uint8_t)std::stoi(e.f[2]));
                std::string detail;
                char buf[256];
                // Game.frameCount keeps counting across R restarts: compare relative to the segment's first row
                if (std::stol(e.f[1]) - segFrameBase != sim->frameCount) {
                    std::snprintf(buf, sizeof buf, "  frame  flash=%s (segment-relative %ld)  sim=%d\n", e.f[1].c_str(),
                                  std::stol(e.f[1]) - segFrameBase, sim->frameCount);
                    detail += buf;
                }
                if ((e.f[20] == "1") != sim->isTimeStop) detail += "  ts     flash=" + e.f[20] + "  sim=" + (sim->isTimeStop ? "1" : "0") + "\n";
                if (e.f.size() >= 27 && (e.f.size() - 21) % 7 == 6) {
                    const size_t c = e.f.size() - 4;
                    const double mine[4] = {sim->camX, sim->camY, sim->dpX, sim->dpY};
                    static const char* fld[4] = {"Level.x", "Level.y", "dp[0]", "dp[1]"};
                    for (int k = 0; k < 4; ++k) {
                        const double fl = FromHex(e.f[c + (size_t)k]);
                        if (!std::isnan(fl) && Bits(fl) != Bits(mine[k])) {
                            std::snprintf(buf, sizeof buf, "  %-7s flash=%.17g  sim=%.17g\n", fld[k], fl, mine[k]);
                            detail += buf;
                        }
                    }
                }
                ++framesCompared;
                if (detail.empty()) {
                    ++framesMatched;
                    ++postDeathFrames;
                } else {
                    ok = false;
                    std::printf("segment %zu (level %d, log line %d): divergence %d ticks after death (tick %s)\n%s\n", si, sg.level,
                                e.line, sim->deadTicks, e.f[0].c_str(), detail.c_str());
                    firstFieldHist["post-death"]++;
                    break;
                }
                if (sim->isTimeStop) {
                    ++winsAfterDeath;
                    break;
                }
                continue;
            }
            uint8_t code = (uint8_t)std::stoi(e.f[2]);
            // Death: Flash logs the 8 playerDiePart* debris bodies from the frame Level.PlayerDie ran.
            bool flashDied = false;
            for (size_t k = 21; k + 7 <= e.f.size(); k += 7)
                if (e.f[k].rfind("playerDiePart", 0) == 0) flashDied = true;
            // Artifact: the logger writes input 0 on the death frame although the game read the keys
            // (every logged death: Flash's velocity matches the previous tick's input). Try the logged
            // input first, then the previous one.
            // (PlayerWin also calls OutControl, so the win row has the same artifact)
            const bool flashWonHere = e.f[20] == "1" && ei > 0 && sg.entries[ei - 1].f[20] != "1";
            if (ei > 0 && (flashDied || flashWonHere) && code == 0 && ei >= 2 && !sg.entries[ei - 1].restart) {
                const uint8_t prevCode = (uint8_t)std::stoi(sg.entries[ei - 1].f[2]);
                if (prevCode != 0) {
                    auto trial = std::make_unique<Sim>(*sim);
                    trial->Tick(code);
                    std::string dummy;
                    const std::vector<int> b0 = Compare(trial->Stats(code), e.f, dummy);
                    bool clean = true;
                    for (int c : b0)
                        if (!ignore[(size_t)c] && !((c == 14 || c == 15 || c == 16) && !trial->playerAlive)) clean = false;
                    if (!clean) {
                        code = prevCode;
                        ++deathInputFromPrev;
                    }
                }
            }
            if (ei > 0) sim->Tick(code);
            FrameStats st = sim->Stats(code);
            std::string detail;
            std::vector<int> bad = Compare(st, e.f, detail);
            const bool simDied = !sim->playerAlive;
            std::vector<int> relevant;
            for (int c : bad) {
                if (ignore[(size_t)c]) continue;
                if ((c == 14 || c == 15 || c == 16) && flashDied && simDied) continue;  // body destroyed in Flash; nC counts the (random) debris
                relevant.push_back(c);
            }
            if (flashDied != simDied) {
                detail += std::string("  death: flash=") + (flashDied ? "died" : "alive") + "  sim=" + (simDied ? "died" : "alive") + "\n";
                relevant.push_back(21);
            }
            int groups = 0;
            if (!CompareExtraBodies(*sim, e.f, detail, groups)) relevant.push_back(21);
            extraGroupsSeen = std::max(extraGroupsSeen, groups);
            ++framesCompared;
            if (relevant.empty()) {
                ++framesMatched;
            } else {
                ok = false;
                std::string key;
                for (int c : relevant) key += std::string(key.empty() ? "" : "+") + (c < 21 ? kField[c] : "otherBodies");
                firstFieldHist[key]++;
                divergeTickHist[std::stoi(e.f[0]) / 100 * 100]++;
                if (shown < verbose) {
                    ++shown;
                    std::printf("segment %zu (level %d, log line %d): first divergence at tick %s (log line %d), input %d\n", si,
                                sg.level, sg.line, e.f[0].c_str(), e.line, code);
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
            if (!sim->playerAlive && !diedCounted) {
                endReason = 1;
                ++deathsMatched;
                diedCounted = true;  // keep going: post-death rows are compared above
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
    if (postDeathFrames || winsAfterDeath)
        std::printf("post-death frames matched (camera, dp, frame, win flag): %ld; wins after death: %ld\n", postDeathFrames, winsAfterDeath);
    if (deathInputFromPrev)
        std::printf("death/win frames logged as input 0 (OutControl), matched with the previous tick's input: %ld\n", deathInputFromPrev);
    if (trailingSkipped)
        std::printf("trailing rows skipped (logged without the frame advancing, input field unreliable): %ld\n", trailingSkipped);
    if (g_cameraFramesChecked)
        std::printf("camera (Level.x/y, dp) compared on %ld frames, ball display matrix on %ld\n", g_cameraFramesChecked,
                    g_matrixFramesChecked);
    if (deathsMatched) std::printf("deaths on the same tick as Flash (player fields exact, debris not simulated): %d\n", deathsMatched);
    if (extraGroupsSeen) std::printf("extra bodies compared per frame: up to %d\n", extraGroupsSeen);
    if (!firstFieldHist.empty()) {
        std::printf("first-divergence fields:\n");
        for (auto& kv : firstFieldHist) std::printf("  %-28s %d\n", kv.first.c_str(), kv.second);
    }
    return diverged ? 1 : 0;
}
