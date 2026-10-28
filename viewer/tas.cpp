// viewer/tas.cpp - see tas.h.
#include "tas.h"
#include "../src/jumpbrute.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace rb;

namespace tas {

// ------------------------------------------------------------------ text formats
bool ParseRLE(const std::string& raw, std::vector<uint8_t>& out, std::string& err) {
    std::string s;
    for (char c : raw)
        if (!std::isspace((unsigned char)c)) s += c;
    out.clear();
    if (s.empty()) return true;
    const std::string letters = "ndweaSqWR";
    long total = 0;
    for (size_t i = 0; i < s.size();) {
        if (letters.find(s[i]) == std::string::npos) {
            err = "Unexpected character in the inputs. Use the game's format, e.g. d12e1w3n5 (n d w e a S q W, R = restart).";
            return false;
        }
        const bool r = s[i] == 'R';
        ++i;
        long count = 0;
        size_t digits = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            count = count * 10 + (s[i] - '0');
            ++i;
            if (++digits > 6) {
                err = "A frame count in the inputs is too large.";
                return false;
            }
        }
        if (!digits && !r) {
            err = "Every letter must be followed by a frame count, e.g. d12.";
            return false;
        }
        total += r ? 1 : count;
        if (total > 200000) {
            err = "The route is longer than 200,000 frames.";
            return false;
        }
    }
    out = DecodeInputs(s);
    return true;
}

std::string RouteToLine(const Route& r) {
    char b[64];
    std::snprintf(b, sizeof b, "L%d C%d G%d I%d ", r.level, r.checkpoint, r.gless ? 1 : 0, r.invincible ? 1 : 0);
    std::string s = b;
    s += r.in.empty() ? "-" : EncodeInputsRLE(r.in);
    for (const auto& [i, w] : r.warps) {
        char t[200];
        std::snprintf(t, sizeof t, " W%d:%.17g,%.17g,%.17g,%.17g,%.17g", i, w.vx, w.vy, w.vr, w.px, w.py);
        s += t;
    }
    return s;
}

bool RouteFromLine(const std::string& line, Route& r, std::string& err) {
    std::istringstream ss(line);
    std::string tok;
    Route o;
    bool haveInputs = false;
    while (ss >> tok) {
        const char c = tok[0];
        if ((c == 'L' || c == 'C' || c == 'G' || c == 'I') && tok.size() > 1 && std::isdigit((unsigned char)tok[1])) {
            const int v = std::atoi(tok.c_str() + 1);
            if (c == 'L') o.level = v;
            else if (c == 'C') o.checkpoint = v;
            else if (c == 'G') o.gless = v != 0;
            else o.invincible = v != 0;
        } else if (c == 'W' && tok.size() > 1 && (std::isdigit((unsigned char)tok[1]))) {
            int idx = 0;
            WarpParams w;
            if (std::sscanf(tok.c_str() + 1, "%d:%lf,%lf,%lf,%lf,%lf", &idx, &w.vx, &w.vy, &w.vr, &w.px, &w.py) != 6) {
                err = "bad warp " + tok;
                return false;
            }
            o.warps[idx] = w;
        } else if (!haveInputs) {
            haveInputs = true;
            if (tok != "-" && !ParseRLE(tok, o.in, err)) return false;
        } else {
            err = "unexpected " + tok;
            return false;
        }
    }
    if (o.level < 1 || o.level > NUM_LEVELS) {
        err = "bad level";
        return false;
    }
    r = o;
    return true;
}

FrameInfo InfoOf(const Sim& s) {
    FrameInfo f;
    f.bx = s.spriteX[s.playerBody];
    f.by = s.spriteY[s.playerBody];
    f.brot = s.spriteRotW[s.playerBody];
    f.alive = s.playerAlive;
    f.won = s.isTimeStop;
    f.frame = s.frameCount;
    f.lastCheck = s.lastCheckNum;
    return f;
}

uint8_t KeysToCode(bool left, bool up, bool right) { return (uint8_t)(4 * left + 2 * up + 1 * right); }

// ------------------------------------------------------------------ Timeline
static std::shared_ptr<LevelTemplate> Template(int level) {
    static std::shared_ptr<LevelTemplate> cache[NUM_LEVELS + 1];
    if (!cache[level]) cache[level] = std::make_shared<LevelTemplate>(level);
    return cache[level];
}

Timeline::Key Timeline::KeyOf(const Sim& s) {
    Key k;
    k.deathFrame = s.deathFrame;
    k.lastCheck = s.lastCheckNum;
    for (int i = 0; i < 4; ++i) k.switchFrame[i] = s.switchFrame[i], k.staticFlag[i] = s.staticFlag[i];
    k.timeStop = s.isTimeStop;
    return k;
}

void Timeline::Advance(Sim& s, int index) const {
    const uint8_t c = route_.in[(size_t)index];
    if (c == IN_RESTART) s.Restart();
    else s.Tick(c);
    const auto w = route_.warps.find(index + 1);
    if (w != route_.warps.end()) s.Warp(w->second.vx, w->second.vy, w->second.vr, w->second.px, w->second.py);
}

void Timeline::Record(const Key& b, const Sim& s, int index) {
    char buf[200];
    const uint8_t c = route_.in[(size_t)index - 1];
    if (c == IN_RESTART) {
        std::snprintf(buf, sizeof buf, "R: restart from checkpoint %d", s.lastCheckNum);
        events_.push_back({index, EventKind::Restart, s.frameCount, buf});
    }
    if (s.deathFrame >= 0 && s.deathFrame != b.deathFrame) {
        std::snprintf(buf, sizeof buf, "death at frame %d", s.deathFrame);
        events_.push_back({index, EventKind::Death, s.deathFrame, buf});
    }
    if (s.lastCheckNum > b.lastCheck && c != IN_RESTART) {
        std::snprintf(buf, sizeof buf, "checkpoint %d at frame %d%s", s.lastCheckNum, s.frameCount, s.playerAlive ? "" : " (dead ball: warp)");
        events_.push_back({index, EventKind::Checkpoint, s.frameCount, buf});
    }
    for (int k = 0; k < 4; ++k) {
        if (c != IN_RESTART && s.switchFrame[k] != b.switchFrame[k] && s.switchFrame[k] != 1) {
            std::snprintf(buf, sizeof buf, "switch pressed (frame %d)%s", s.frameCount, s.playerAlive ? "" : " by the dead ball");
            events_.push_back({index, EventKind::Switch, s.frameCount, buf});
        }
        if (s.staticFlag[k] != b.staticFlag[k] && s.staticFlag[k]) {
            const char* what = k == 2 ? "wrong way: checkpoint reset to the start" : "switch state saved for restarts";
            std::snprintf(buf, sizeof buf, "%s (frame %d)%s", what, s.frameCount, s.playerAlive ? "" : ", dead ball");
            events_.push_back({index, EventKind::Static, s.frameCount, buf});
        }
    }
    if (s.isTimeStop && !b.timeStop) {
        std::snprintf(buf, sizeof buf, "FLAG at frame %d (%.3f s)", s.winFrame, s.winFrame / 31.0);
        std::string text = buf;
        if (s.deathFrame >= 0) text += DeathWarpFinishValid(s.deathFrame, s.winFrame) ? ", death warp" : ", death warp TOO LATE for a real run";
        events_.push_back({index, EventKind::Flag, s.winFrame, text});
    }
    if (route_.warps.count(index)) {
        std::snprintf(buf, sizeof buf, "warp (/set) at frame %d", s.frameCount);
        events_.push_back({index, EventKind::Warp, s.frameCount, buf});
    }
}

void Timeline::Build(const Route& r) {
    route_ = r;
    if (!tpl_ || tplLevel_ != r.level) tpl_ = Template(r.level), tplLevel_ = r.level;
    auto s = std::make_unique<Sim>();
    s->Load(tpl_.get(), r.checkpoint);
    s->gless = r.gless;
    s->invincible = r.invincible;
    const auto w0 = route_.warps.find(0);
    if (w0 != route_.warps.end()) s->Warp(w0->second.vx, w0->second.vy, w0->second.vr, w0->second.px, w0->second.py);
    snaps_.clear();
    info_.clear();
    events_.clear();
    snaps_.push_back(std::make_unique<Sim>(*s));
    info_.push_back(InfoOf(*s));
    if (route_.warps.count(0)) events_.push_back({0, EventKind::Warp, 0, "warp (/set) at the start"});
    for (int i = 0; i < (int)route_.in.size(); ++i) {
        const Key k = KeyOf(*s);
        Advance(*s, i);
        Record(k, *s, i + 1);
        info_.push_back(InfoOf(*s));
        if ((i + 1) % K == 0) snaps_.push_back(std::make_unique<Sim>(*s));
    }
    tail_ = std::move(s);
}

void Timeline::Truncate(int n) {
    n = std::max(0, std::min(n, size()));
    if (n == size()) return;
    route_.in.resize((size_t)n);
    for (auto it = route_.warps.begin(); it != route_.warps.end();) it = it->first > n ? route_.warps.erase(it) : std::next(it);
    info_.resize((size_t)n + 1);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event& e) { return e.index > n; }), events_.end());
    snaps_.resize((size_t)n / K + 1);
    tail_.reset();  // StateAt(size()) would hand back the old tail
    tail_ = StateAt(n);
}

void Timeline::Append(uint8_t code) {
    route_.in.push_back(code);
    const int i = size() - 1;
    const Key k = KeyOf(*tail_);
    Advance(*tail_, i);
    Record(k, *tail_, i + 1);
    info_.push_back(InfoOf(*tail_));
    if ((i + 1) % K == 0) snaps_.push_back(std::make_unique<Sim>(*tail_));
}

void Timeline::SetWarp(int index, const WarpParams& w) {
    Route r = route_;
    r.warps[index] = w;
    Build(r);
}

std::unique_ptr<Sim> Timeline::StateAt(int pos) const {
    pos = std::max(0, std::min(pos, size()));
    if (pos == size() && tail_) return std::make_unique<Sim>(*tail_);
    const int j = std::min(pos / K, (int)snaps_.size() - 1);
    auto s = std::make_unique<Sim>(*snaps_[(size_t)j]);
    for (int i = j * K; i < pos; ++i) Advance(*s, i);
    return s;
}

int Timeline::deathFrame() const {
    for (auto it = events_.rbegin(); it != events_.rend(); ++it)
        if (it->kind == EventKind::Death) return it->frame;
    return -1;
}
int Timeline::winFrame() const {
    for (const Event& e : events_)
        if (e.kind == EventKind::Flag) return e.frame;
    return -1;
}
int Timeline::winIndex() const {
    for (const Event& e : events_)
        if (e.kind == EventKind::Flag) return e.index;
    return -1;
}
bool Timeline::warpValid() const {
    int death = -1;
    for (const Event& e : events_) {
        if (e.kind == EventKind::Restart) death = -1;
        if (e.kind == EventKind::Death) death = e.frame;
        if (e.kind == EventKind::Flag) return DeathWarpFinishValid(death, e.frame);
    }
    return true;
}

// ------------------------------------------------------------------ BruteEnv (BruteForcer.as)
void BruteEnv::Start(const std::vector<uint8_t>& prefix, const Sim& next) {
    const Body& b = next.world.bodies[next.playerBody];
    ypos0 = b.xf.position.y;
    vy0 = b.linearVelocity.y;
    jumpstart = (int)prefix.size();
    untilJump = prefix;
    started = true;
}

void BruteEnv::End(const std::vector<uint8_t>& full, const Sim& now) {
    x = (int)full.size() - jumpstart - 1;
    maxypos = (double)x * (x + 1) / 180 + vy0 / 30 * x + ypos0;
    const double ypos = now.world.bodies[now.playerBody].xf.position.y;
    currYOffsets = (int)std::floor((maxypos - ypos) / yOffset + 0.5);  // Math.round
    lrDuringJump.clear();
    for (size_t i = (size_t)jumpstart; i < full.size(); ++i) lrDuringJump.push_back(full[i] & 5);
    active = true;
    choice = 0;
}

void BruteEnv::Adjust(int change, const Timeline& base) {
    currYOffsets += change;
    Compute(base);
}

void BruteEnv::Compute(const Timeline& base) {
    const double PI = 3.141592653589793;
    cands.clear();
    const JumpBrute model(x + 1, vy0);
    for (int up = 0; up <= model.MaxCount(); ++up) {
        if (currYOffsets < 0 || !model.Allows(up, (uint64_t)currYOffsets)) continue;
        BruteCandidate c;
        c.in = untilJump;
        c.in.insert(c.in.end(), lrDuringJump.begin(), lrDuringJump.end());
        model.Place(up, (uint64_t)currYOffsets, [&](int i) { c.in[untilJump.size() + (size_t)i] |= IN_U; });
        c.predictedVy = x / 3.0 + vy0 - up * 40 / (147 * PI);
        cands.push_back(std::move(c));
    }
    // rbview: simulate every candidate from the take-off state
    const Route& r = base.route();
    const bool samePrefix = base.size() >= jumpstart && std::equal(untilJump.begin(), untilJump.end(), r.in.begin());
    std::unique_ptr<Sim> start;
    if (samePrefix) start = base.StateAt(jumpstart);
    for (BruteCandidate& c : cands) {
        Route cr = r;
        cr.in = c.in;
        for (auto it = cr.warps.begin(); it != cr.warps.end();) it = it->first > jumpstart ? cr.warps.erase(it) : std::next(it);
        if (start) {
            Sim s = *start;
            for (int i = jumpstart; i < (int)c.in.size(); ++i) {
                if (c.in[(size_t)i] == IN_RESTART) s.Restart();
                else s.Tick(c.in[(size_t)i]);
            }
            c.endY = s.world.bodies[s.playerBody].xf.position.y;
            c.endVy = s.world.bodies[s.playerBody].linearVelocity.y;
            c.alive = s.playerAlive;
        } else {
            Timeline t;
            t.Build(cr);
            const Sim& s = t.Tail();
            c.endY = s.world.bodies[s.playerBody].xf.position.y;
            c.endVy = s.world.bodies[s.playerBody].linearVelocity.y;
            c.alive = s.playerAlive;
        }
    }
    if (choice >= (int)cands.size()) choice = 0;
}

// ------------------------------------------------------------------ Session
Route Session::Prefix() const {
    Route r = route();
    r.in.resize((size_t)pos);
    for (auto it = r.warps.begin(); it != r.warps.end();) it = it->first > pos ? r.warps.erase(it) : std::next(it);
    return r;
}

void Session::Load(const Route& r, int newPos) {
    tl.Build(r);
    pos = std::max(0, std::min(newPos, tl.size()));
    cur = tl.StateAt(pos);
    tickAcc = 0;
}

void Session::Seek(int p) {
    pos = std::max(0, std::min(p, tl.size()));
    cur = tl.StateAt(pos);
}

void Session::Backup() {
    const Route& r = route();
    if (r.in.empty() && r.warps.empty()) return;
    if (!backups.empty() && backups.front() == r) return;
    backups.push_front(r);
    while (backups.size() > 500) backups.pop_back();
}

void Session::TruncateAt(int n) {
    if (n < tl.size()) Backup();
    tl.Truncate(n);
    pos = tl.size();
    cur = std::make_unique<Sim>(tl.Tail());
}

void Session::Step(uint8_t keys) {
    if (!cur) return;
    const bool wasOver = cur->isTimeStop;
    if (InPlayback()) {
        tl.Advance(*cur, pos);
        ++pos;
        if (!InPlayback() && !cur->isTimeStop) {
            // The mod hands control back here (and resets the game speed to 1x; rbview keeps the speed you chose).
            tickAcc = 0;
            if (stopAtEnd) {
                paused = true;
                feedback = "Playback ended: paused at the end of the route";
            } else {
                feedback = "Playback ended: recording your keys from here";
            }
        }
    } else if (!cur->isTimeStop) {  // after the flag the game stops recording (and counting)
        tl.Append(keys & 7);
        pos = tl.size();
        *cur = tl.Tail();
    }
    if (cur->isTimeStop && !wasOver) {
        paused = true;  // nothing happens after the flag: stop the clock
        feedback = "Flag on frame " + std::to_string(cur->winFrame) + ": the run is over";
    }
}

void Session::RemoveWarps() {
    Route r = route();
    if (r.warps.empty()) return;
    Backup();
    r.warps.clear();
    const int p = pos;
    Load(r, p);
}

int Session::Run(double frames, uint8_t keys, int maxTicks) {
    if (paused || !cur) return 0;
    tickAcc += frames * speed;
    if (fastPlayback && InPlayback()) tickAcc = std::max(tickAcc, 64.0 * frames);
    int n = 0;
    while (tickAcc >= 1 && n < maxTicks && !paused) {
        tickAcc -= 1;  // gameTick - 1, then Update (the end of a playback resets gameTick to 0)
        Step(keys);
        ++n;
    }
    if (n >= maxTicks) tickAcc = 0;
    return n;
}

void Session::FrameAdvance(uint8_t keys) {
    if (speed == 0 || paused) Step(keys);
    else paused = true, tickAcc = 0;  // rbview: Space while running pauses first (the mod ignores it)
}

void Session::SpeedUp() {
    if (speed == 0) speed = 1.0 / 32;
    else if (speed != 64) speed *= 2;
    tickAcc = 0;
}

void Session::SpeedDown() {
    if (speed > 1.0 / 30) speed /= 2;
    else if (speed == 1.0 / 32) speed = 0;
    tickAcc = 0;
}

void Session::TogglePause() {
    paused = !paused;
    tickAcc = 0;
}

void Session::DropPlayback() {
    if (!InPlayback()) return;
    TruncateAt(pos);
    paused = true;  // the mod sets tPause = 0
    feedback = "Playback inputs dropped";
}

void Session::UndoFrame() {
    if (!(speed == 0 || paused) || pos == 0) return;
    TruncateAt(pos - 1);
}

void Session::Rewind(int frames) {
    if (!(speed == 0 || paused)) return;
    TruncateAt(pos > frames ? pos - frames : 0);
}

void Session::RestartLevel() {
    Backup();
    Route r = route();
    r.in.clear();
    r.warps.clear();
    Load(r, 0);
    feedback.clear();
}

void Session::CheckpointRestart() {
    if (InPlayback()) TruncateAt(pos);
    tl.Append(IN_RESTART);
    pos = tl.size();
    *cur = tl.Tail();
}

void Session::SaveSlot(int n) {
    slots[n] = Prefix();
    slotUsed[n] = true;
    feedback = "State " + std::to_string(n) + " saved!";
}

void Session::LoadRoute(const Route& r) {
    Backup();
    if (realtimeLoad) {
        Load(r, 0);
        paused = false;  // tPause = 1: the playback runs
    } else {
        Load(r, (int)r.in.size());
    }
}

void Session::LoadSlot(int n) {
    if (!slotUsed[n]) return;
    LoadRoute(slots[n]);
    feedback = "State " + std::to_string(n) + " loaded!";
}

void Session::ImportSlot(int n, const Route& r, bool load) {
    slots[n] = r;
    slotUsed[n] = true;
    feedback = "State " + std::to_string(n) + " imported!";
    if (load) {
        realtimeLoad = true;  // the mod switches realtime playback on for Shift+digit imports
        LoadRoute(r);
        feedback = "State " + std::to_string(n) + " loaded!";
    }
}

void Session::SaveNamed(const std::string& name) { named.push_back({name, Prefix()}); }

void Session::SetFlags(bool gless, bool invincible) {
    Route r = route();
    if (r.gless == gless && r.invincible == invincible) return;
    r.gless = gless;
    r.invincible = invincible;
    const int p = pos;
    Load(r, p);
}

void Session::AddWarp(const WarpParams& w) {
    tl.SetWarp(pos, w);
    cur = tl.StateAt(pos);
}

void Session::BruteStart(uint8_t keys) {
    Sim next = *cur;  // the mod runs one Update with the keys of the moment (during a playback: the route's next input)
    next.Tick(InPlayback() ? (uint8_t)(route().in[(size_t)pos] & 7) : (uint8_t)(keys & 7));
    brute.Start(Prefix().in, next);
    feedback = "Data for start of jump collected!";
}

void Session::BruteEnd() {
    if (!brute.started) {
        feedback = "Press B at the take-off first";
        return;
    }
    if (InPlayback()) TruncateAt(pos);
    paused = true;
    brute.End(Prefix().in, *cur);
    brute.Compute(tl);
    feedback = "Entered brute force environment!";
}

void Session::BruteApply() {
    if (brute.cands.empty()) return;
    Route r = route();
    r.in = brute.cands[(size_t)brute.choice].in;
    for (auto it = r.warps.begin(); it != r.warps.end();) it = it->first > brute.jumpstart ? r.warps.erase(it) : std::next(it);
    Backup();
    Load(r, (int)r.in.size());  // saveState[0] + LoadState(0): always at once
}

// ------------------------------------------------------------------ persistence
bool Session::SaveStatesFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# rbview savestates: slot<TAB>N<TAB>route | named<TAB>name<TAB>route | backup<TAB>route\n";
    for (int i = 0; i < 10; ++i)
        if (slotUsed[i]) f << "slot\t" << i << "\t" << RouteToLine(slots[i]) << "\n";
    for (const NamedState& n : named) {
        std::string name = n.name;
        std::replace(name.begin(), name.end(), '\t', ' ');
        std::replace(name.begin(), name.end(), '\n', ' ');
        f << "named\t" << name << "\t" << RouteToLine(n.route) << "\n";
    }
    for (const Route& r : backups) f << "backup\t" << RouteToLine(r) << "\n";
    return (bool)f;
}

bool Session::LoadStatesFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line, err;
    named.clear();
    backups.clear();
    for (bool& u : slotUsed) u = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) {
            const size_t b = line.find('\t', a);
            c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        Route r;
        if (c[0] == "slot" && c.size() == 3) {
            const int n = std::atoi(c[1].c_str());
            if (n >= 0 && n < 10 && RouteFromLine(c[2], r, err)) slots[n] = r, slotUsed[n] = true;
        } else if (c[0] == "named" && c.size() == 3) {
            if (RouteFromLine(c[2], r, err)) named.push_back({c[1], r});
        } else if (c[0] == "backup" && c.size() == 2) {
            if (RouteFromLine(c[1], r, err)) backups.push_back(r);
        }
    }
    return true;
}

// ------------------------------------------------------------------ commands (CONFIG.processConfigData, Commands.as)
struct KeyName {
    const char* name;
    int vk;
};
static const KeyName kKeys[] = {
    {"BACKSPACE", 8},  {"TAB", 9},       {"ENTER", 13},       {"SHIFT", 16},        {"CONTROL", 17},     {"CAPS_LOCK", 20},
    {"ESCAPE", 27},    {"SPACE", 32},    {"PAGE_UP", 33},     {"PAGE_DOWN", 34},    {"END", 35},         {"HOME", 36},
    {"LEFT", 37},      {"UP", 38},       {"RIGHT", 39},       {"DOWN", 40},         {"INSERT", 45},      {"DELETE", 46},
    {"SEMICOLON", 186}, {"EQUAL", 187},  {"COMMA", 188},      {"MINUS", 189},       {"PERIOD", 190},     {"SLASH", 191},
    {"BACKQUOTE", 192}, {"LEFTBRACKET", 219}, {"BACKSLASH", 220}, {"RIGHTBRACKET", 221}, {"QUOTE", 222}};

int KeyNameToVk(const std::string& raw) {
    std::string n;
    for (char c : raw) n += (char)std::toupper((unsigned char)c);
    if (n.empty()) return -1;
    if (n[0] >= '0' && n[0] <= '9') return std::atoi(n.c_str());  // the mod takes a number as a key code
    if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return n[0];
    if (n.rfind("NUMBER_", 0) == 0 && n.size() == 8 && std::isdigit((unsigned char)n[7])) return '0' + (n[7] - '0');
    if (n.rfind("NUMPAD_", 0) == 0 && n.size() == 8 && std::isdigit((unsigned char)n[7])) return 96 + (n[7] - '0');
    if (n[0] == 'F' && n.size() <= 3 && std::isdigit((unsigned char)n[1])) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return 111 + f;
    }
    for (const KeyName& k : kKeys)
        if (n == k.name) return k.vk;
    return -1;
}

std::string VkName(int vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= 96 && vk <= 105) return "NUMPAD_" + std::to_string(vk - 96);
    if (vk >= 112 && vk <= 123) return "F" + std::to_string(vk - 111);
    for (const KeyName& k : kKeys)
        if (vk == k.vk) return k.name;
    return std::to_string(vk);
}

CommandResult RunCommand(Session& s, const std::string& raw, int& decimals) {
    CommandResult res;
    std::string line;
    for (char c : raw) line += (char)std::tolower((unsigned char)c);
    std::vector<std::string> args;
    {
        std::istringstream ss(line);
        std::string t;
        while (ss >> t) args.push_back(t);
    }
    if (args.empty()) return res;
    if (args[0] == "/set") {
        if (!s.cur) return res;
        const Body& b = s.cur->world.bodies[s.cur->playerBody];
        double v[5] = {b.linearVelocity.x, b.linearVelocity.y, b.angularVelocity * 0.35, b.xf.position.x, b.xf.position.y};
        static const char* names[5] = {"-vx", "-vy", "-vr", "-px", "-py"};
        int r = 0;
        for (int i = 0; i < 5; ++i) {
            const auto it = std::find(args.begin() + 1, args.end(), names[i]);
            if (it != args.end() && it + 1 != args.end()) {
                v[i] = std::strtod((it + 1)->c_str(), nullptr);
                ++r;
            }
        }
        s.AddWarp({v[0], v[1], v[2], v[3], v[4]});
        res.feedback = "Set " + std::to_string(r) + " parameters.";
        return res;
    }
    if (args[0] == "/remap") {
        if (args.size() >= 2 && args[1] == "reset") {
            res.remapReset = true;
            res.feedback = "Successfully reset remappings";
            return res;
        }
        if (args.size() < 3) {
            res.feedback = "Malformed remap request";
            return res;
        }
        const int from = KeyNameToVk(args[1]), to = KeyNameToVk(args[2]);
        if (from < 0 || to < 0) {
            res.feedback = "Malformed remap request";
            return res;
        }
        res.remapFrom = from, res.remapTo = to;
        res.feedback = "Successfully remapped " + std::to_string(from) + " to " + std::to_string(to);
        return res;
    }
    if (args[0] != "/config") {
        res.feedback = "Malformed Configuration Settings";
        return res;
    }
    const int v = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
    if (args.size() > 1 && args[1] == "decimals") {
        decimals = std::max(0, std::min(17, v));
        res.feedback = "Config Accepted: DECIMALS : " + std::to_string(decimals);
    } else if (args.size() > 1 && args[1] == "reversedw") {
        res.feedback = "Config Accepted: REVERSEDW : " + std::to_string(v) + " (no effect in rbview)";
    } else if (args.size() > 1 && args[1] == "mathspikes") {
        if (v < -1 || v > 2) {
            res.feedback = "mathspikes: 0, 1 or 2 (-1: spikes off)";
        } else {
            GetDisplayConfig().mathSpikes = v;
            res.displayChanged = true;
            res.feedback = "Config Accepted: MATHSPIKES : " + std::to_string(v);
        }
    } else {
        res.feedback = "Unknown Configuration Parameter";
    }
    return res;
}

}  // namespace tas
