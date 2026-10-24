// smartforce.cpp - `rbsim smartforce`: this is basically red ball regex
#include "redball.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace rb;

namespace {

using Seq = std::vector<uint8_t>;
using Options = std::vector<Seq>;
constexpr int MAX_CONDS = 16;
constexpr size_t MAX_LEN = 1000000;

double StringBytes(size_t len) { return 72.0 + (double)len; }
double Bytes(const Options& o) {
    double b = 24;
    for (const Seq& q : o) b += StringBytes(q.size());
    return b;
}

struct Parser {
    static constexpr int MAX_DEPTH = 200;
    static constexpr double MAX_WORK = 4e8;
    std::string s;
    size_t i = 0;
    std::string err;
    double budget = 4e9, used = 0, work = 0;
    int depth = 0;
    void Fail(const std::string& m) {
        if (err.empty()) err = m + " at position " + std::to_string(i + 1);
    }
    bool Spend(double steps) {
        work += steps;
        if (work <= MAX_WORK) return true;
        Fail("the pattern is too big to count (more than 400 million steps): use smaller ranges or fewer nested parts");
        return false;
    }
    bool Charge(double bytes) {
        used += bytes;
        if (used <= budget) return true;
        Fail("the pattern's counting tables alone need more than --maxmem");
        return false;
    }
    void Skip() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    }
    bool Eat(char c) {
        Skip();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool AtEnd() {
        Skip();
        return i >= s.size();
    }
    bool Int(long& v) {
        Skip();
        if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
        v = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) v = std::min(v * 10 + (s[i++] - '0'), 100000000L);
        return true;
    }
    void Range(long& lo, long& hi, long def) {
        if (!Int(lo)) {
            lo = hi = def;
            return;
        }
        hi = lo;
        if (s.compare(i, 2, "..") == 0) {
            i += 2;
            if (!Int(hi)) Fail("number expected after ..");
        }
        if (hi < lo) Fail("empty range");
    }
    bool Code(uint8_t& c) {
        static const char L[] = "ndweaSqW";
        Skip();
        if (i >= s.size() || !s[i]) return false;
        const char* p = std::strchr(L, s[i]);
        if (!p) return false;
        c = (uint8_t)(p - L);
        ++i;
        return true;
    }
};

struct Tap {
    uint8_t code;
    long lo, hi;
};

constexpr uint64_t SAT = 1ULL << 62;
uint64_t SatAdd(uint64_t a, uint64_t b) { return a >= SAT || b >= SAT - a ? SAT : a + b; }
uint64_t SatMul(uint64_t a, uint64_t b) { return a && b > SAT / a ? SAT : a * b; }
uint64_t BinomU(long n, long k) {
    if (k < 0 || k > n) return 0;
    k = std::min(k, n - k);
    unsigned __int128 r = 1;
    for (long j = 1; j <= k; ++j) {
        r = r * (unsigned __int128)(n - k + j) / (unsigned __int128)j;
        if (r >= SAT) return SAT;
    }
    return (uint64_t)r;
}

uint64_t NextBinom(uint64_t c, long f, long k) {
    if (c >= SAT) return BinomU(f, k + 1);
    const unsigned __int128 n = (unsigned __int128)c * (unsigned __int128)(f - k) / (unsigned __int128)(k + 1);
    return n >= SAT ? SAT : (uint64_t)n;
}

struct Scratch {
    std::vector<char> used;
    std::vector<size_t> freePos;
};

constexpr double DEDUPE_BYTES = 17;

uint64_t SeqHash(const Seq& q) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ q.size();
    size_t i = 0;
    for (; i + 8 <= q.size(); i += 8) {
        uint64_t w;
        std::memcpy(&w, q.data() + i, 8);
        h = (h ^ w) * 0xC2B2AE3D27D4EB4FULL, h ^= h >> 29;
    }
    for (; i < q.size(); ++i) h = (h ^ q[i]) * 0x165667B19E3779F9ULL, h ^= h >> 31;
    return h ^ (h >> 32);
}

// Keeps the first of equal strings, in order.
void DedupeKeepOrder(Options& o) {
    if (o.size() < 2) return;
    std::vector<std::pair<uint64_t, size_t>> h(o.size());
    for (size_t i = 0; i < o.size(); ++i) h[i] = {SeqHash(o[i]), i};
    std::sort(h.begin(), h.end());
    std::vector<char> drop(o.size(), 0);
    for (size_t i = 0, j; i < h.size(); i = j) {
        for (j = i + 1; j < h.size() && h[j].first == h[i].first; ++j)
            for (size_t y = i; y < j; ++y)
                if (!drop[h[y].second] && o[h[j].second] == o[h[y].second]) {
                    drop[h[j].second] = 1;
                    break;
                }
    }
    size_t w = 0;
    for (size_t r = 0; r < o.size(); ++r)
        if (!drop[r]) {
            if (w != r) o[w] = std::move(o[r]);
            ++w;
        }
    o.resize(w);
}

void PlaceTaps(const std::vector<Tap>& taps, size_t i, bool add, Seq& q, std::vector<char>& used, Options& o) {
    if (i == taps.size()) {
        o.push_back(q);
        return;
    }
    std::vector<size_t> freePos;
    for (size_t j = 0; j < q.size(); ++j)
        if (!used[j]) freePos.push_back(j);
    const long F = (long)freePos.size();
    for (long k = taps[i].lo; k <= std::min(taps[i].hi, F); ++k) {
        std::vector<long> idx((size_t)k);
        for (long j = 0; j < k; ++j) idx[(size_t)j] = j;
        for (;;) {
            Seq saved = q;
            for (long j : idx) {
                const size_t at = freePos[(size_t)j];
                q[at] = add ? (uint8_t)(q[at] | taps[i].code) : taps[i].code;
                used[at] = 1;
            }
            PlaceTaps(taps, i + 1, add, q, used, o);
            for (long j : idx) used[freePos[(size_t)j]] = 0;
            q.swap(saved);
            long j = k - 1;
            while (j >= 0 && idx[(size_t)j] == F - k + j) --j;
            if (j < 0) break;
            ++idx[(size_t)j];
            for (long t = j + 1; t < k; ++t) idx[(size_t)t] = idx[(size_t)t - 1] + 1;
        }
    }
}

// Strings are ordered by length, then as written; cnt[L] = strings of length L.
struct Node {
    enum Kind { RUN, SEQ, ALT, PLACE } kind = RUN;
    uint8_t code = 0;
    long lo = 0, hi = 0;
    bool filtered = false;
    long flo = 0, fhi = LONG_MAX;
    std::vector<std::unique_ptr<Node>> kids;
    std::vector<Tap> taps;
    bool add = false;
    std::vector<uint64_t> cnt, cum;
    std::vector<std::vector<uint64_t>> suf, ways;
    bool stored = false;
    Options list;
    std::vector<uint64_t> off;
    uint64_t total = 0;
    double charged = 0, tableBytes = 0;

    void Restrict(long a, long b) {
        filtered = true;
        flo = std::max(flo, a), fhi = std::min(fhi, b);
        if (kind == ALT || kind == PLACE)
            for (auto& k : kids) k->Restrict(a, b);
    }

    uint64_t At(const std::vector<uint64_t>& v, long L) const { return L >= 0 && (size_t)L < v.size() ? v[(size_t)L] : 0; }

    void Trim(std::vector<uint64_t>& v) {
        while (v.size() > 1 && v.back() == 0) v.pop_back();
    }

    double Tables() const {
        double b = 8.0 * (double)(cnt.size() + cum.size() + off.size());
        for (const auto& v : suf) b += 8.0 * (double)v.size();
        for (const auto& v : ways) b += 8.0 * (double)v.size();
        return b;
    }

    bool Count(Parser& p) {
        cnt.assign(1, 0);
        if (kind == RUN) {
            cnt.assign((size_t)hi + 1, 0);
            for (long l = lo; l <= hi; ++l) cnt[(size_t)l] = 1;
        } else if (kind == SEQ) {
            suf.assign(kids.size() + 1, std::vector<uint64_t>(1, 1));
            for (size_t j = kids.size(); j-- > 0;) {
                const std::vector<uint64_t>& a = kids[j]->cnt;
                const std::vector<uint64_t>& b = suf[j + 1];
                if (a.size() + b.size() - 2 > MAX_LEN) {
                    p.Fail("pattern strings longer than 1,000,000 frames");
                    return false;
                }
                std::vector<size_t> nza, nzb;
                for (size_t x = 0; x < a.size(); ++x)
                    if (a[x]) nza.push_back(x);
                for (size_t y = 0; y < b.size(); ++y)
                    if (b[y]) nzb.push_back(y);
                if (!p.Spend((double)nza.size() * (double)nzb.size())) return false;
                std::vector<uint64_t> r(a.size() + b.size() - 1, 0);
                for (size_t x : nza)
                    for (size_t y : nzb) r[x + y] = SatAdd(r[x + y], SatMul(a[x], b[y]));
                Trim(r);
                suf[j] = std::move(r);
            }
            cnt = suf[0];
        } else if (kind == PLACE) {
            cnt = kids[0]->cnt;
            const size_t maxLen = cnt.size() - 1;
            ways.assign(taps.size() + 1, std::vector<uint64_t>(maxLen + 1, 0));
            for (size_t f = 0; f <= maxLen; ++f) ways[taps.size()][f] = 1;
            for (size_t t = taps.size(); t-- > 0;)
                for (size_t f = 0; f <= maxLen; ++f) {
                    const long kmax = std::min(taps[t].hi, (long)f);
                    if (taps[t].lo > kmax) continue;
                    uint64_t c = BinomU((long)f, taps[t].lo);
                    long k = taps[t].lo;
                    for (; k <= kmax && ways[t][f] < SAT; c = NextBinom(c, (long)f, k), ++k)
                        ways[t][f] = SatAdd(ways[t][f], SatMul(c, ways[t + 1][f - (size_t)k]));
                    if (!p.Spend((double)(k - taps[t].lo + 1))) return false;
                }
            for (size_t L = 0; L <= maxLen; ++L) cnt[L] = SatMul(cnt[L], ways[0][L]);
        } else {
            for (const auto& k : kids) {
                if (k->cnt.size() > cnt.size()) cnt.resize(k->cnt.size(), 0);
                for (size_t L = 0; L < k->cnt.size(); ++L) cnt[L] = SatAdd(cnt[L], k->cnt[L]);
            }
        }
        if (filtered)
            for (size_t L = 0; L < cnt.size(); ++L)
                if ((long)L < flo || (long)L > fhi) cnt[L] = 0;
        Trim(cnt);
        total = 0;
        cum.assign(cnt.size() + 1, 0);
        for (size_t L = 0; L < cnt.size(); ++L) total = cum[L + 1] = SatAdd(total, cnt[L]);
        tableBytes = Tables();
        return p.Charge(tableBytes);
    }

    void GetByLen(long L, uint64_t r, Seq& out, Scratch& sc) const {
        if (stored) {
            const Seq& q = list[(size_t)(off[(size_t)L] + r)];
            out.insert(out.end(), q.begin(), q.end());
            return;
        }
        if (kind == RUN) {
            out.insert(out.end(), (size_t)L, code);
        } else if (kind == SEQ) {
            long rest = L;
            for (size_t j = 0; j < kids.size(); ++j) {
                const Node& kid = *kids[j];
                const std::vector<uint64_t>& next = suf[j + 1];
                for (long l = 0; l <= std::min((long)kid.cnt.size() - 1, rest); ++l) {
                    const uint64_t c = kid.cnt[(size_t)l], s = At(next, rest - l);
                    if (!c || !s) continue;
                    const uint64_t block = SatMul(c, s);
                    if (r >= block) {
                        r -= block;
                        continue;
                    }
                    kid.GetByLen(l, r / s, out, sc);
                    r %= s;
                    rest -= l;
                    break;
                }
            }
        } else if (kind == ALT) {
            for (const auto& k : kids) {
                const uint64_t c = At(k->cnt, L);
                if (r < c) {
                    k->GetByLen(L, r, out, sc);
                    return;
                }
                r -= c;
            }
        } else {
            const uint64_t w = ways[0][(size_t)L];
            const size_t start = out.size();
            kids[0]->GetByLen(L, r / w, out, sc);
            r %= w;
            sc.used.assign((size_t)L, 0);
            for (size_t t = 0; t < taps.size(); ++t) {
                sc.freePos.clear();
                for (size_t j = 0; j < (size_t)L; ++j)
                    if (!sc.used[j]) sc.freePos.push_back(j);
                const long F = (long)sc.freePos.size();
                uint64_t ck = BinomU(F, taps[t].lo);
                for (long k = taps[t].lo; k <= std::min(taps[t].hi, F); ck = NextBinom(ck, F, k), ++k) {
                    const uint64_t inner = ways[t + 1][(size_t)(F - k)];
                    const uint64_t block = SatMul(ck, inner);
                    if (r >= block) {
                        r -= block;
                        continue;
                    }
                    uint64_t comb = r / inner;
                    r %= inner;
                    long x = 0;
                    for (long j = 0; j < k; ++j, ++x) {
                        for (;;) {
                            const uint64_t c = BinomU(F - x - 1, k - j - 1);
                            if (comb < c) break;
                            comb -= c;
                            ++x;
                        }
                        const size_t at = sc.freePos[(size_t)x];
                        out[start + at] = add ? (uint8_t)(out[start + at] | taps[t].code) : taps[t].code;
                        sc.used[at] = 1;
                    }
                    break;
                }
            }
        }
    }

    void Get(uint64_t i, Seq& out, Scratch& sc) const {
        out.clear();
        if (stored) {
            out = list[(size_t)i];
            return;
        }
        const long L = (long)(std::upper_bound(cum.begin(), cum.end(), i) - cum.begin()) - 1;
        GetByLen(L, i - cum[(size_t)L], out, sc);
    }

    bool KidsStored() const {
        for (const auto& k : kids)
            if (!k->stored) return false;
        return true;
    }

    bool Steals() const { return kind == ALT && kids.size() == 1 && kids[0]->stored; }

    void SetList(Options&& o) {
        list = std::move(o);
        stored = true;
        cnt.assign(1, 0);
        for (const Seq& q : list) {
            if (q.size() >= cnt.size()) cnt.resize(q.size() + 1, 0);
            ++cnt[q.size()];
        }
        off.assign(cnt.size() + 1, 0);
        for (size_t L = 0; L < cnt.size(); ++L) off[L + 1] = off[L] + cnt[L];
        cum = off;
        total = list.size();
        suf.clear(), ways.clear();
    }

    bool Materialize(Parser& p) {
        Options o;
        if (kind == RUN) {
            for (long l = lo; l <= hi; ++l) o.push_back(Seq((size_t)l, code));
        } else if (kind == PLACE && KidsStored()) {
            o.reserve((size_t)total);
            for (const Seq& b : kids[0]->list) {
                Seq q = b;
                std::vector<char> used(b.size(), 0);
                PlaceTaps(taps, 0, add, q, used, o);
            }
        } else if (Steals()) {
            o.swap(kids[0]->list);
        } else {
            if (!p.Spend((double)total)) return false;
            o.reserve((size_t)total);
            Seq q;
            Scratch sc;
            for (uint64_t i = 0; i < total; ++i) {
                Get(i, q, sc);
                o.push_back(q);
            }
        }
        if (filtered) o.erase(std::remove_if(o.begin(), o.end(), [&](const Seq& q) { return (long)q.size() < flo || (long)q.size() > fhi; }), o.end());
        if (!Steals()) DedupeKeepOrder(o);
        SetList(std::move(o));
        return true;
    }

    void Release(Parser& p) {
        for (auto& k : kids) k->Release(p);
        p.used -= charged + tableBytes;
        charged = tableBytes = 0;
        Options().swap(list);
        std::vector<std::vector<uint64_t>>().swap(suf), std::vector<std::vector<uint64_t>>().swap(ways);
    }

    void Recharge(Parser& p) {
        p.used -= charged + tableBytes;
        charged = stored ? Bytes(list) : 0;
        tableBytes = Tables();
        p.used += charged + tableBytes;
    }

    bool Build(Parser& p) {
        for (auto& k : kids)
            if (!k->Build(p)) return false;
        if (!Count(p)) return false;
        if (total == 0) return true;
        double need = 0;
        if (!Steals())
            for (size_t L = 0; L < cnt.size(); ++L) need += (double)cnt[L] * (StringBytes(L) + DEDUPE_BYTES);
        if (total < SAT && p.used + need <= p.budget) {
            if (!Materialize(p)) return false;
            for (auto& k : kids) k->Release(p);
            Recharge(p);
        }
        return true;
    }
};

std::unique_ptr<Node> ParseSeq(Parser& p, const char* stops);

std::unique_ptr<Node> ParseItem(Parser& p) {
    auto n = std::make_unique<Node>();
    struct Depth {
        Parser& p;
        explicit Depth(Parser& q) : p(q) { ++p.depth; }
        ~Depth() { --p.depth; }
    } depth(p);
    if (p.depth > Parser::MAX_DEPTH) {
        p.Fail("brackets nested more than 200 deep");
        return n;
    }
    if (p.Code(n->code)) {
        n->kind = Node::RUN;
        p.Range(n->lo, n->hi, 1);
        if (n->hi > 100000) p.Fail("run longer than 100000 frames");
        return n;
    }
    if (p.Eat('(')) {
        n->kind = Node::ALT;
        do n->kids.push_back(ParseSeq(p, ")|>"));
        while (p.err.empty() && p.Eat('|'));
        if (p.err.empty() && !p.Eat(')')) p.Fail("')' expected");
        if (p.err.empty() && p.Eat('=')) {
            long a, b;
            p.Range(a, b, -1);
            if (a < 0) p.Fail("length expected after =");
            else n->Restrict(a, b);
        }
        if (!n->filtered && n->kids.size() == 1) return std::move(n->kids[0]);
        return n;
    }
    if (p.Eat('<')) {
        n->kind = Node::PLACE;
        n->kids.push_back(ParseSeq(p, ")|>:+"));
        if (!p.err.empty()) return n;
        n->add = p.Eat('+');
        if (!n->add && !p.Eat(':')) {
            p.Fail("expected the form <BASE:a3>, <BASE:a1..3,w1> or <BASE+a3>");
            return n;
        }
        do {
            Tap t;
            if (!p.Code(t.code)) {
                p.Fail("tap letter expected");
                return n;
            }
            p.Range(t.lo, t.hi, 1);
            n->taps.push_back(t);
        } while (p.err.empty() && p.Eat(','));
        if (p.err.empty() && !p.Eat('>')) p.Fail("'>' expected");
        return n;
    }
    if (p.Eat('R')) p.Fail("R (restart) is not supported");
    else p.Fail(std::string("unexpected '") + p.s[p.i] + "'");
    return n;
}

std::unique_ptr<Node> ParseSeq(Parser& p, const char* stops) {
    auto n = std::make_unique<Node>();
    n->kind = Node::SEQ;
    while (p.err.empty() && !p.AtEnd() && !std::strchr(stops, p.s[p.i])) n->kids.push_back(ParseItem(p));
    if (n->kids.size() == 1) return std::move(n->kids[0]);
    return n;
}

struct Segment {
    std::unique_ptr<Node> node;
    uint64_t size() const { return node->total; }
    bool generated() const { return !node->stored; }
    void Get(uint64_t i, Seq& out, Scratch& sc) const { node->Get(i, out, sc); }
};

std::vector<Segment> ParsePattern(const std::string& s, double budget, double& usedBytes, std::string& err) {
    Parser p;
    p.s = s;
    p.budget = budget;
    std::vector<Segment> segs;
    while (p.err.empty() && !p.AtEnd()) {
        if (std::strchr(")|>", p.s[p.i])) {
            p.Fail(std::string("unexpected '") + p.s[p.i] + "'");
            break;
        }
        Segment sg;
        sg.node = ParseItem(p);
        if (!p.err.empty() || !sg.node->Build(p)) break;
        if (sg.node->total == 0) {
            p.Fail("this part of the pattern has no strings (no expansion of the required length, or more taps than frames)");
            break;
        }
        if (sg.node->total >= SAT) {
            p.Fail("this part of the pattern has more than 4.6e18 strings");
            break;
        }
        if (sg.node->stored && sg.node->total == 1 && !segs.empty() && segs.back().node->stored && segs.back().node->total == 1) {
            Node& prev = *segs.back().node;
            Options joined(1, prev.list[0]);
            joined[0].insert(joined[0].end(), sg.node->list[0].begin(), sg.node->list[0].end());
            sg.node->Release(p);
            prev.SetList(std::move(joined));
            prev.Recharge(p);
        } else {
            segs.push_back(std::move(sg));
        }
    }
    err = p.err;
    usedBytes = p.used;
    return segs;
}

enum Kind { K_WIN, K_DEAD, K_ALIVE, K_GROUND, K_CONTACT, K_CMP };
enum Quantity { Q_X, Q_Y, Q_VX, Q_VY, Q_PR, Q_VR, Q_FLAG, Q_COUNT };
const char* const kQuantity[Q_COUNT] = {"px", "py", "vx", "vy", "pr", "vr", "flag"};

struct Cond {
    Kind kind = K_WIN;
    int q = 0;
    std::string op, name, text;
    double v = 0;
    int lo = 1, hi = INT_MAX;
};

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool ParseFrame(const std::string& s, int& v) {
    char* end = nullptr;
    const long x = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || x < 1) return false;
    v = (int)std::min(x, (long)INT_MAX);
    return true;
}

bool ParseCond(const std::string& tok, Cond& c, std::string& err) {
    c.text = Trim(tok);
    std::string pred = c.text;
    const size_t at = pred.find('@');
    if (at != std::string::npos) {
        const std::string w = Trim(pred.substr(at + 1));
        pred = Trim(pred.substr(0, at));
        const size_t dd = w.find("..");
        bool ok;
        if (dd == std::string::npos) ok = ParseFrame(w, c.lo) && ParseFrame(w, c.hi);
        else
            ok = (dd == 0 || ParseFrame(w.substr(0, dd), c.lo)) && (dd + 2 == w.size() || ParseFrame(w.substr(dd + 2), c.hi));
        if (!ok || c.hi < c.lo) {
            err = "bad frame window in '" + c.text + "' (use @F, @A..B, @..B or @A..)";
            return false;
        }
    }
    if (pred == "win") c.kind = K_WIN;
    else if (pred == "dead") c.kind = K_DEAD;
    else if (pred == "alive") c.kind = K_ALIVE;
    else if (pred == "ground") c.kind = K_GROUND;
    else if (pred.compare(0, 7, "contact") == 0 && (pred.size() == 7 || Trim(pred.substr(7))[0] == '=')) {
        c.kind = K_CONTACT;
        const std::string r = Trim(pred.substr(7));
        if (!r.empty()) c.name = Trim(r.substr(1));
        if (!r.empty() && c.name.empty()) {
            err = "contact= needs a body name in '" + c.text + "' (--names lists them)";
            return false;
        }
    } else {
        const size_t pos = pred.find_first_of("<>=");
        if (pos == std::string::npos) {
            err = "unknown condition '" + c.text + "'";
            return false;
        }
        c.kind = K_CMP;
        const std::string q = Trim(pred.substr(0, pos));
        c.q = -1;
        for (int k = 0; k < Q_COUNT; ++k)
            if (q == kQuantity[k]) c.q = k;
        c.op = pred.substr(pos, pos + 1 < pred.size() && pred[pos + 1] == '=' && pred[pos] != '=' ? 2 : 1);
        const std::string val = Trim(pred.substr(pos + c.op.size()));
        char* end = nullptr;
        c.v = std::strtod(val.c_str(), &end);
        if (c.q < 0 || val.empty() || *end) {
            err = "bad comparison '" + c.text + "' (quantities: px py pr vx vy vr flag)";
            return false;
        }
    }
    return true;
}

double Value(Sim& s, int q) {
    const Body& b = s.world.bodies[s.playerBody];
    switch (q) {
        case Q_X: return b.xf.position.x;
        case Q_Y: return b.xf.position.y;
        case Q_VX: return b.linearVelocity.x;
        case Q_VY: return b.linearVelocity.y;
        case Q_PR: return std::fmod(b.sweep.a * 180 / AS3_PI, 360);
        case Q_VR: return b.angularVelocity * 0.35;
        default: return -s.TargetOverlap(s.GoalTarget());
    }
}

bool Touches(const Sim& s, const std::string& name) {
    const World& w = s.world;
    if (name.empty()) return w.listener.count > 0;
    for (int32_t i = 0; i < w.listener.count; ++i) {
        const int32_t b = w.listener.bodies[i];
        if (b == w.groundBody) {
            if (name == "ground") return true;
            continue;
        }
        const int32_t tag = w.bodies[b].userTag - 100;
        if (tag >= 0 && tag < (int32_t)s.tpl->bodyNames.size() && s.tpl->bodyNames[(size_t)tag] == name) return true;
    }
    return false;
}

bool Holds(const Cond& c, Sim& s, bool won) {
    const bool alive = s.playerAlive;
    switch (c.kind) {
        case K_WIN: return won;
        case K_DEAD: return !alive;
        case K_ALIVE: return alive;
        case K_GROUND: return alive && (s.probeC || s.probeL || s.probeR);
        case K_CONTACT: return alive && Touches(s, c.name);
        case K_CMP: {
            if (!alive && c.q != Q_FLAG) return false;
            const double v = Value(s, c.q);
            if (c.op == "<") return v < c.v;
            if (c.op == "<=") return v <= c.v;
            if (c.op == ">") return v > c.v;
            if (c.op == ">=") return v >= c.v;
            return v == c.v;
        }
    }
    return false;
}

bool CanHoldDead(const Cond& c) { return c.kind == K_WIN || c.kind == K_DEAD || (c.kind == K_CMP && c.q == Q_FLAG); }

struct Key {
    uint64_t a, b;
    bool operator==(const Key& o) const { return a == o.a && b == o.b; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return (size_t)k.a; }
};

struct Field {
    std::string name;
    size_t off, size, elem = 0, count = 1;
    const std::vector<Field>* sub = nullptr;
};

struct Layout {
    std::vector<uint8_t> isDouble;
    std::vector<Field> simFields, worldFields, bodyFields, shapeFields, contactFields, pairFields, proxyFields;

    Layout() {
        auto sp = std::make_unique<Sim>();
        const Sim& s = *sp;
        const World& w = s.world;
        const char* base = reinterpret_cast<const char*>(&s);
        isDouble.assign(sizeof(Sim) / 8 + 1, 0);
        auto D = [&](const double& d) { isDouble[(size_t)(reinterpret_cast<const char*>(&d) - base) / 8] = 1; };
        auto V = [&](const Vec2& v) { D(v.x), D(v.y); };
        auto M = [&](const Mat22& m) { V(m.col1), V(m.col2); };
        auto X = [&](const XForm& x) { V(x.position), M(x.R); };
        auto J = [&](const Jacobian& j) { V(j.linear1), D(j.angular1), V(j.linear2), D(j.angular2); };
        V(w.gravity), D(w.inv_dt0);
        for (const Body& b : w.bodies) {
            X(b.xf), V(b.sweep.localCenter), D(b.sweep.a), V(b.sweep.c), D(b.sweep.a0), V(b.sweep.c0), D(b.sweep.t0);
            V(b.linearVelocity), V(b.force);
            for (const double* d : {&b.angularVelocity, &b.torque, &b.mass, &b.invMass, &b.I, &b.invI, &b.linearDamping,
                                    &b.angularDamping, &b.sleepTime})
                D(*d);
        }
        for (const Shape& sh : w.shapes) D(sh.sweepRadius), D(sh.density), D(sh.friction), D(sh.restitution);
        for (const Contact& c : w.contacts) {
            D(c.toi), D(c.friction), D(c.restitution), V(c.manifold.normal), X(c.evalXf1), X(c.evalXf2);
            for (const ManifoldPoint& p : c.manifold.points)
                V(p.localPoint1), V(p.localPoint2), D(p.separation), D(p.normalImpulse), D(p.tangentImpulse);
        }
        for (const Joint& j : w.joints) {
            V(j.localAnchor1), V(j.localAnchor2), V(j.u), V(j.localXAxis1), V(j.localYAxis1), V(j.pivotForce), M(j.pivotMass);
            J(j.linearJacobian), J(j.motorJacobian);
            for (const double* d : {&j.inv_dt, &j.length, &j.frequencyHz, &j.dampingRatio, &j.impulse, &j.gamma, &j.bias, &j.mass,
                                    &j.refAngle, &j.linearMass, &j.force, &j.angularMass, &j.torque, &j.motorMass, &j.motorForce,
                                    &j.limitForce, &j.limitPositionImpulse, &j.lowerTranslation, &j.upperTranslation,
                                    &j.maxMotorForce, &j.motorSpeed, &j.referenceAngle, &j.lowerAngle, &j.upperAngle,
                                    &j.maxMotorTorque})
                D(*d);
        }
        V(w.bp.worldAABB.lowerBound), V(w.bp.worldAABB.upperBound), V(w.bp.quantizationFactor);
        for (const double* d : {&s.camX, &s.camY, &s.camTargetX, &s.camTargetY, &s.dpX, &s.dpY, &s.winMargin}) D(*d);
        for (int i = 0; i < CAP_BODIES; ++i)
            D(s.spriteX[i]), D(s.spriteY[i]), D(s.spriteRot[i]), D(s.spriteRotW[i]), D(s.syncX[i]), D(s.syncY[i]), D(s.syncA[i]);

        auto F = [](std::vector<Field>& v, const void* owner, const char* name, const void* m, size_t size) {
            v.push_back({name, (size_t)(static_cast<const char*>(m) - static_cast<const char*>(owner)), size});
        };
#define RB_F(vec, obj, member) F(vec, &obj, #member, &obj.member, sizeof(obj.member))
        const Body& b = w.bodies[0];
        RB_F(bodyFields, b, xf), RB_F(bodyFields, b, sweep), RB_F(bodyFields, b, linearVelocity), RB_F(bodyFields, b, force);
        RB_F(bodyFields, b, angularVelocity), RB_F(bodyFields, b, torque), RB_F(bodyFields, b, mass), RB_F(bodyFields, b, invMass);
        RB_F(bodyFields, b, I), RB_F(bodyFields, b, invI), RB_F(bodyFields, b, linearDamping), RB_F(bodyFields, b, angularDamping);
        RB_F(bodyFields, b, sleepTime), RB_F(bodyFields, b, flags), RB_F(bodyFields, b, type), RB_F(bodyFields, b, prev);
        RB_F(bodyFields, b, next), RB_F(bodyFields, b, shapeList), RB_F(bodyFields, b, shapeCount), RB_F(bodyFields, b, contactList);
        RB_F(bodyFields, b, jointList), RB_F(bodyFields, b, userTag), RB_F(bodyFields, b, inWorld);
        const Shape& sh = w.shapes[0];
        RB_F(shapeFields, sh, geom), RB_F(shapeFields, sh, type), RB_F(shapeFields, sh, body), RB_F(shapeFields, sh, next);
        RB_F(shapeFields, sh, sweepRadius), RB_F(shapeFields, sh, density), RB_F(shapeFields, sh, friction);
        RB_F(shapeFields, sh, restitution), RB_F(shapeFields, sh, filter), RB_F(shapeFields, sh, isSensor), RB_F(shapeFields, sh, proxyId);
        const Contact& c = w.contacts[0];
        RB_F(contactFields, c, kind), RB_F(contactFields, c, shape1), RB_F(contactFields, c, shape2), RB_F(contactFields, c, prev);
        RB_F(contactFields, c, next), RB_F(contactFields, c, node), RB_F(contactFields, c, toi), RB_F(contactFields, c, friction);
        RB_F(contactFields, c, restitution), RB_F(contactFields, c, flags), RB_F(contactFields, c, manifoldCount);
        for (int k = 0; k < settings::maxManifoldPoints; ++k) {
            const ManifoldPoint& p = c.manifold.points[k];
            const std::string pre = "manifold.points[" + std::to_string(k) + "].";
            for (auto [n, m, z] : {std::tuple<const char*, const void*, size_t>{"localPoint1", &p.localPoint1, sizeof(Vec2)},
                                   {"localPoint2", &p.localPoint2, sizeof(Vec2)}, {"separation", &p.separation, 8},
                                   {"normalImpulse", &p.normalImpulse, 8}, {"tangentImpulse", &p.tangentImpulse, 8},
                                   {"key", &p.key, 4}})
                contactFields.push_back({pre + n, (size_t)(static_cast<const char*>(m) - reinterpret_cast<const char*>(&c)), z});
        }
        F(contactFields, &c, "manifold.normal", &c.manifold.normal, sizeof(Vec2));
        F(contactFields, &c, "manifold.pointCount", &c.manifold.pointCount, 4);
        RB_F(contactFields, c, nextFree), RB_F(contactFields, c, evalValid), RB_F(contactFields, c, evalXf1), RB_F(contactFields, c, evalXf2);
        const Pair& pr = w.bp.pm.pairs[0];
        RB_F(pairFields, pr, proxyId1), RB_F(pairFields, pr, proxyId2), RB_F(pairFields, pr, status), RB_F(pairFields, pr, next);
        RB_F(pairFields, pr, userData);
        const Proxy& px = w.bp.proxyPool[0];
        RB_F(proxyFields, px, lowerBounds), RB_F(proxyFields, px, upperBounds), RB_F(proxyFields, px, overlapCount);
        RB_F(proxyFields, px, timeStamp), RB_F(proxyFields, px, userData);
        auto A = [&](std::vector<Field>& v, const void* owner, const char* name, const void* first, size_t elem, size_t count,
                     const std::vector<Field>* sub) {
            v.push_back({name, (size_t)(static_cast<const char*>(first) - static_cast<const char*>(owner)), elem * count, elem, count, sub});
        };
        A(worldFields, &w, "bodies", &w.bodies[0], sizeof(Body), CAP_BODIES, &bodyFields);
        A(worldFields, &w, "shapes", &w.shapes[0], sizeof(Shape), CAP_SHAPES, &shapeFields);
        A(worldFields, &w, "contacts", &w.contacts[0], sizeof(Contact), CAP_CONTACTS, &contactFields);
        A(worldFields, &w, "joints", &w.joints[0], sizeof(Joint), CAP_JOINTS, nullptr);
        A(worldFields, &w, "bp.pm.pairs", &w.bp.pm.pairs[0], sizeof(Pair), CAP_PAIRS, &pairFields);
        A(worldFields, &w, "bp.pm.hashTable", &w.bp.pm.hashTable[0], 4, CAP_PAIR_TABLE, nullptr);
        A(worldFields, &w, "bp.pm.pairBuffer", &w.bp.pm.pairBuffer[0], sizeof(BufferedPair), CAP_PAIR_BUFFER, nullptr);
        A(worldFields, &w, "bp.proxyPool", &w.bp.proxyPool[0], sizeof(Proxy), CAP_PROXIES, &proxyFields);
        A(worldFields, &w, "bp.bounds", &w.bp.bounds[0][0], sizeof(Bound), 4 * CAP_PROXIES, nullptr);
        A(worldFields, &w, "bp.queryResults", &w.bp.queryResults[0], 4, CAP_PROXIES, nullptr);
        A(worldFields, &w, "listener.bodies", &w.listener.bodies[0], 4, CAP_PLAYER_CONTACTS, nullptr);
        A(worldFields, &w, "deadAt", &w.deadAt[0], 4, CAP_BODIES, nullptr);
        RB_F(worldFields, w, bodyList), RB_F(worldFields, w, contactList), RB_F(worldFields, w, jointList);
        RB_F(worldFields, w, bodyCount), RB_F(worldFields, w, contactCount), RB_F(worldFields, w, freeContact);
        RB_F(worldFields, w, numContactSlots), RB_F(worldFields, w, stepCount), RB_F(worldFields, w, inv_dt0);
        F(worldFields, &w, "bp.pm.pairCount", &w.bp.pm.pairCount, 4), F(worldFields, &w, "bp.pm.freePair", &w.bp.pm.freePair, 4);
        F(worldFields, &w, "bp.pm.pairBufferCount", &w.bp.pm.pairBufferCount, 4);
        F(worldFields, &w, "bp.freeProxy", &w.bp.freeProxy, 4), F(worldFields, &w, "bp.proxyCount", &w.bp.proxyCount, 4);
        F(worldFields, &w, "bp.timeStamp", &w.bp.timeStamp, 4), F(worldFields, &w, "bp.queryResultCount", &w.bp.queryResultCount, 4);
        F(worldFields, &w, "listener.count", &w.listener.count, 4);
        A(simFields, &s, "world", &s.world, sizeof(World), 1, &worldFields);
        A(simFields, &s, "spriteX", &s.spriteX[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "spriteY", &s.spriteY[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "spriteRot", &s.spriteRot[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "spriteRotW", &s.spriteRotW[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "spDrawnX", &s.spDrawnX[0], 8, CAP_SPIKES, nullptr);
        A(simFields, &s, "spDrawnY", &s.spDrawnY[0], 8, CAP_SPIKES, nullptr);
        A(simFields, &s, "lvBody", &s.lvBody[0], 4, LV_VARS, nullptr);
        A(simFields, &s, "lvInt", &s.lvInt[0], 4, LV_VARS, nullptr);
        RB_F(simFields, s, camX), RB_F(simFields, s, camY), RB_F(simFields, s, camTargetX), RB_F(simFields, s, camTargetY);
        RB_F(simFields, s, dpX), RB_F(simFields, s, dpY), RB_F(simFields, s, winMargin), RB_F(simFields, s, frameCount);
        RB_F(simFields, s, deadTicks), RB_F(simFields, s, deathFrame), RB_F(simFields, s, winFrame), RB_F(simFields, s, cpFrame);
        RB_F(simFields, s, probeC), RB_F(simFields, s, probeL), RB_F(simFields, s, probeR), RB_F(simFields, s, upIdle);
        RB_F(simFields, s, displayUncertain), RB_F(simFields, s, syncValid);
        A(simFields, &s, "syncX", &s.syncX[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "syncY", &s.syncY[0], 8, CAP_BODIES, nullptr);
        A(simFields, &s, "syncA", &s.syncA[0], 8, CAP_BODIES, nullptr);
#undef RB_F
    }

    static std::string Find(const std::vector<Field>& fs, size_t off) {
        for (const Field& f : fs) {
            if (off < f.off || off >= f.off + f.size) continue;
            if (f.elem == 0) return f.name;
            const size_t inner = (off - f.off) % f.elem;
            std::string n = f.name + (f.count > 1 ? "[]" : "");
            if (!f.sub) return n;
            const std::string s = Find(*f.sub, inner);
            return n + "." + (s.empty() ? "(padding)" : s);
        }
        return "";
    }
    std::string Name(size_t off) const {
        const std::string n = Find(simFields, off);
        return n.empty() ? "sim+" + std::to_string(off) : n;
    }
};

const Layout& GetLayout() {
    static const Layout l;
    return l;
}

struct Span {
    size_t a, b;  // bytes [a, b) of a Sim
};

// Bodies, shapes, contact slots and joints are only appended, so slots past the counts are as loaded.
size_t LiveSpans(const Sim& s, const Sim& t, Span (&out)[5]) {
    static_assert(sizeof(Body) % 8 == 0 && sizeof(Shape) % 8 == 0 && sizeof(Contact) % 8 == 0 && sizeof(Joint) % 8 == 0,
                  "slots are whole 8-byte words");
    const char* o = reinterpret_cast<const char*>(&s);
    const World &w = s.world, &v = t.world;
    Span d[4];
    auto dead = [&](const void* first, size_t elem, int32_t n1, int32_t n2, int cap) {
        const size_t n = (size_t)std::max(0, std::min(cap, (int)std::max(n1, n2)));
        const size_t a = (size_t)(static_cast<const char*>(first) - o);
        return Span{a + elem * n, a + elem * (size_t)cap};
    };
    d[0] = dead(w.bodies, sizeof(Body), w.numBodies, v.numBodies, CAP_BODIES);
    d[1] = dead(w.shapes, sizeof(Shape), w.numShapes, v.numShapes, CAP_SHAPES);
    d[2] = dead(w.contacts, sizeof(Contact), w.numContactSlots, v.numContactSlots, CAP_CONTACTS);
    d[3] = dead(w.joints, sizeof(Joint), w.numJoints, v.numJoints, CAP_JOINTS);
    std::sort(d, d + 4, [](const Span& x, const Span& y) { return x.a < y.a; });
    size_t n = 0, at = 0;
    for (const Span& x : d) {
        if (x.a > at) out[n++] = {at, x.a};
        at = std::max(at, x.b);
    }
    if (at < sizeof(Sim)) out[n++] = {at, sizeof(Sim)};
    return n;
}

// Byte-exact copy between states of one run.
void CopySim(Sim& dst, const Sim& src) {
    Span sp[5];
    const size_t n = LiveSpans(src, dst, sp);
    for (size_t i = 0; i < n; ++i)
        std::memcpy(reinterpret_cast<char*>(&dst) + sp[i].a, reinterpret_cast<const char*>(&src) + sp[i].a, sp[i].b - sp[i].a);
}

// full: also the slots past the counts (--merge-report reads them).
void Canonicalize(const Sim& src, Sim& dst, bool full = false) {
    if (full) std::memcpy(static_cast<void*>(&dst), static_cast<const void*>(&src), sizeof(Sim));
    else {
        Span sp[5];
        const size_t n = LiveSpans(src, src, sp);
        for (size_t i = 0; i < n; ++i)
            std::memcpy(reinterpret_cast<char*>(&dst) + sp[i].a, reinterpret_cast<const char*>(&src) + sp[i].a, sp[i].b - sp[i].a);
    }
    dst.probeC = dst.probeL = dst.probeR = dst.upIdle = false;
    dst.displayUncertain = 0;
    std::memset(dst.syncX, 0, sizeof dst.syncX), std::memset(dst.syncY, 0, sizeof dst.syncY);
    std::memset(dst.syncA, 0, sizeof dst.syncA), std::memset(dst.syncValid, 0, sizeof dst.syncValid);
    World& w = dst.world;
    bool isFree[CAP_CONTACTS] = {};
    for (int32_t c = w.freeContact, n = 0; c >= 0 && c < CAP_CONTACTS && n < CAP_CONTACTS; c = src.world.contacts[c].nextFree, ++n)
        isFree[c] = true;
    for (int32_t c = 0; c < w.numContactSlots; ++c) {
        Contact& ct = w.contacts[c];
        if (isFree[c]) {
            const int32_t nf = ct.nextFree;
            std::memset(static_cast<void*>(&ct), 0, sizeof ct);
            ct.nextFree = nf;
            continue;
        }
        ct.evalValid = false;
        std::memset(static_cast<void*>(&ct.evalXf1), 0, sizeof(XForm)), std::memset(static_cast<void*>(&ct.evalXf2), 0, sizeof(XForm));
        const int32_t pc = std::max(0, std::min(ct.manifold.pointCount, (int32_t)settings::maxManifoldPoints));
        for (int32_t k = pc; k < settings::maxManifoldPoints; ++k)
            std::memset(static_cast<void*>(&ct.manifold.points[k]), 0, sizeof(ManifoldPoint));
        if (pc == 0) std::memset(static_cast<void*>(&ct.manifold.normal), 0, sizeof(Vec2));
    }
    for (int32_t i = std::max(0, w.listener.count); i < CAP_PLAYER_CONTACTS; ++i) w.listener.bodies[i] = 0;
    for (int32_t i = std::max(0, w.bp.queryResultCount); i < CAP_PROXIES; ++i) w.bp.queryResults[i] = 0;
    for (int32_t i = std::max(0, w.bp.pm.pairBufferCount); i < CAP_PAIR_BUFFER; ++i) w.bp.pm.pairBuffer[i] = {0, 0};
}

uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t Avalanche(uint64_t h) {
    h ^= h >> 33, h *= 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29, h *= 0x165667B19E3779F9ULL;
    return h ^ (h >> 32);
}

struct Hasher {
    uint64_t a, b;
    explicit Hasher(uint64_t seed) : a(0x9E3779B97F4A7C15ULL ^ seed), b(0x27D4EB2F165667C5ULL + seed) {}
    void Mix(uint64_t w) {
        a = Rotl(a + w * 0xC2B2AE3D27D4EB4FULL, 31) * 0x9E3779B185EBCA87ULL;
        b = Rotl(b ^ (w * 0x165667B19E3779F9ULL), 27) * 0x85EBCA77C2B2AE63ULL + 0x27D4EB2F165667C5ULL;
    }
    Key Done() const { return {Avalanche(a), Avalanche(b ^ a)}; }
};

uint64_t Quantize(uint64_t bits, double tol) {
    double v;
    std::memcpy(&v, &bits, 8);
    if (!(tol > 0)) return bits;
    const double q = std::floor(v / tol);
    if (!(std::fabs(q) < 9.0e18)) return bits;
    return (uint64_t)(int64_t)q ^ 0xA5A5A5A5A5A5A5A5ULL;
}

// Hashes only the words that differ from the level start; doubles on the --tolerance grid.
Key StateKey(const Sim& c, const Sim& base, const int* met, size_t nconds, double tol) {
    static_assert(sizeof(Sim) % 8 == 0, "Sim is hashed in 8-byte words");
    typedef uint64_t __attribute__((may_alias)) Word;
    const Word* p = reinterpret_cast<const Word*>(&c);
    const Word* q = reinterpret_cast<const Word*>(&base);
    const std::vector<uint8_t>& dbl = GetLayout().isDouble;
    uint64_t lane[4] = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
    Span sp[5];
    const size_t spans = LiveSpans(c, c, sp);
    for (size_t s = 0; s < spans; ++s) {
        const size_t end = sp[s].b / 8;
        for (size_t i = sp[s].a / 8; i < end; i += 8) {
            const size_t e = std::min(i + 8, end);
            uint64_t changed = 0;
#if defined(__SSE2__)
            if (e - i == 8) {
                const auto equal = [&](size_t j) {
                    const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + j));
                    const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + j));
                    return (uint64_t)_mm_movemask_epi8(_mm_cmpeq_epi32(x, y));
                };
                changed = ~(equal(i) | (equal(i + 2) << 16) | (equal(i + 4) << 32) | (equal(i + 6) << 48));
                changed = (changed | (changed >> 4)) & 0x0101010101010101ULL;
            } else
#endif
                for (size_t j = i; j < e; ++j)
                    if (p[j] != q[j]) changed |= 1ULL << (8 * (j - i));
            while (changed) {
                const size_t j = i + (size_t)__builtin_ctzll(changed) / 8;
                changed &= changed - 1;
                uint64_t w = p[j];
                if (dbl[j] && tol > 0) {
                    w = Quantize(w, tol);
                    if (w == Quantize(q[j], tol)) continue;
                }
                uint64_t& l = lane[j & 3];
                l = Rotl(l + (w ^ (j * 0x9E3779B185EBCA87ULL)) * 0xC2B2AE3D27D4EB4FULL, 31) * 0x9E3779B185EBCA87ULL;
            }
        }
    }
    Hasher h(lane[0] ^ Rotl(lane[1], 17));
    h.Mix(lane[2]), h.Mix(lane[3]), h.Mix(lane[1]);
    for (size_t j = 0; j < nconds; ++j) h.Mix((uint64_t)(uint32_t)met[j]);
    return h.Done();
}

uint64_t Place(size_t seg, uint64_t rest) { return seg ^ (rest * 0x9E3779B97F4A7C15ULL); }
Key PlaceKey(const Key& s, uint64_t at) {
    Hasher h(at);
    h.Mix(s.a), h.Mix(s.b);
    return h.Done();
}

Key CoarseKey(const Sim& c, size_t seg, const int* met, size_t nconds) {
    Hasher h(seg ^ 0x1234567ULL);
    auto q = [&](double v) { h.Mix(std::fabs(v) < 1e8 ? (uint64_t)(int64_t)std::floor(v * 1e7) : 0); };
    h.Mix((uint64_t)c.frameCount), h.Mix((uint64_t)c.playerAlive), h.Mix((uint64_t)c.isTimeStop);
    for (int32_t b = 0; b < c.world.numBodies; ++b) {
        const Body& B = c.world.bodies[b];
        h.Mix((uint64_t)B.inWorld);
        q(B.xf.position.x), q(B.xf.position.y), q(B.sweep.a), q(B.linearVelocity.x), q(B.linearVelocity.y), q(B.angularVelocity);
    }
    for (size_t j = 0; j < nconds; ++j) h.Mix((uint64_t)(uint32_t)met[j]);
    return h.Done();
}

// One write per block: the console draws slowly while all cores are busy.
void PrintText(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

void Say(const char* fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    const int n = std::vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    std::string s(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) std::vsnprintf(&s[0], s.size() + 1, fmt, b);
    va_end(b);
    PrintText(s);
}

std::string FreeName(const std::string& want) {
    auto exists = [](const std::string& p) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    };
    if (!exists(want)) return want;
    const size_t slash = want.find_last_of("/\\"), dot = want.find_last_of('.');
    const size_t cut = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : want.size();
    for (int k = 2;; ++k) {
        const std::string p = want.substr(0, cut) + "-" + std::to_string(k) + want.substr(cut);
        if (!exists(p)) return p;
    }
}

struct Report {
    std::mutex m;
    std::unordered_map<Key, std::pair<Key, std::unique_ptr<Sim>>, KeyHash> reps;
    std::map<std::string, std::pair<long, double>> diffs;
    long pairs = 0, stored = 0;

    void Check(const Sim& canon, const Key& fine, const Key& coarse, double tol) {
        std::lock_guard<std::mutex> g(m);
        auto it = reps.find(coarse);
        if (it == reps.end()) {
            if (stored >= 256) return;
            auto copy = std::make_unique<Sim>();
            std::memcpy(static_cast<void*>(copy.get()), static_cast<const void*>(&canon), sizeof(Sim));
            reps.emplace(coarse, std::make_pair(fine, std::move(copy)));
            ++stored;
            return;
        }
        if (it->second.first == fine) return;
        ++pairs;
        const Layout& L = GetLayout();
        const unsigned char* x = reinterpret_cast<const unsigned char*>(it->second.second.get());
        const unsigned char* y = reinterpret_cast<const unsigned char*>(&canon);
        std::set<std::string> seenHere;
        for (size_t i = 0; i + 8 <= sizeof(Sim); i += 8) {
            if (!std::memcmp(x + i, y + i, 8)) continue;
            double dx = 0, dy = 0;
            std::memcpy(&dx, x + i, 8), std::memcpy(&dy, y + i, 8);
            const bool isD = L.isDouble[i / 8];
            std::string n = L.Name(i) + (isD ? "" : " (non-double)");
            if (isD && tol > 0 && std::fabs(dx - dy) < tol) n = "doubles within tolerance, other side of a grid line";
            auto& e = diffs[n];
            if (seenHere.insert(n).second) ++e.first;
            if (isD) e.second = std::max(e.second, std::fabs(dx - dy));
        }
    }
    void Print() {
        Say("merge report: %ld stored states, %ld unmerged states whose bodies match a stored one to 1e-7\n", stored, pairs);
        std::vector<std::pair<std::string, std::pair<long, double>>> v(diffs.begin(), diffs.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (const auto& e : v)
            Say("  %6ld  %-60s  max |d| %.3g\n", e.second.first, e.first.c_str(), e.second.second);
    }
};

double PhysicalMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (double)m.ullTotalPhys : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? (double)pages * (double)size : 0;
#endif
}

std::atomic<bool> g_stop{false};
void OnInterrupt(int) {
    g_stop = true;
    std::signal(SIGINT, SIG_DFL);
}

constexpr double SEEN_ENTRY_BYTES = 48, ROW_BYTES = 320;

// Claimed state keys: 64 locked open-addressing shards.
struct Seen {
    static constexpr int N = 64;
    struct Shard {
        std::mutex m;
        std::vector<Key> slot;  // {0, 0}: empty
        size_t used = 0;
        bool zero = false;      // the key {0, 0} itself
    };
    Shard s[N];
    size_t cap = SIZE_MAX;
    std::atomic<size_t> count{0};
    std::atomic<bool> full{false};

    static Key* Find(std::vector<Key>& t, const Key& k) {
        const size_t mask = t.size() - 1;
        for (size_t i = (size_t)k.a & mask;; i = (i + 1) & mask)
            if (t[i] == k || (!t[i].a && !t[i].b)) return &t[i];
    }
    bool Claim(const Key& k) {
        Shard& sh = s[k.b % N];
        std::lock_guard<std::mutex> g(sh.m);
        const bool isZero = !k.a && !k.b;
        Key* at = isZero || sh.slot.empty() ? nullptr : Find(sh.slot, k);
        if (isZero ? sh.zero : at && *at == k) return false;
        if (count.load() >= cap) {
            full = true;
            return true;
        }
        ++count;
        if (isZero) return sh.zero = true;
        if ((sh.used + 1) * 4 > sh.slot.size() * 3) {
            std::vector<Key> grown(std::max<size_t>(1024, 2 * sh.slot.size()), Key{0, 0});
            for (const Key& x : sh.slot)
                if (x.a || x.b) *Find(grown, x) = x;
            sh.slot.swap(grown);
            at = Find(sh.slot, k);
        }
        *at = k, ++sh.used;
        return true;
    }
};

struct Match {
    int frame = 0;
    bool alive = true;
    double x = 0, y = 0, vx = 0, vy = 0, flag = 0, count = 0;
    std::string route;
};

// --sortby: highest first (lowest with -); default and ties: frame, then flag.
enum SortBy { BY_DEFAULT, BY_FRAME, BY_PX, BY_PY, BY_VX, BY_VY, BY_FLAG };
SortBy g_sortBy = BY_DEFAULT;
bool g_sortLowFirst = false;

bool Before(const Match& a, const Match& b) {
    if (g_sortBy != BY_DEFAULT) {
        const bool needsAlive = g_sortBy >= BY_PX && g_sortBy <= BY_VY;
        if (needsAlive && a.alive != b.alive) return a.alive;
        const double x = g_sortBy == BY_FRAME ? a.frame : g_sortBy == BY_PX ? a.x : g_sortBy == BY_PY ? a.y : g_sortBy == BY_VX ? a.vx
                       : g_sortBy == BY_VY ? a.vy : a.flag;
        const double y = g_sortBy == BY_FRAME ? b.frame : g_sortBy == BY_PX ? b.x : g_sortBy == BY_PY ? b.y : g_sortBy == BY_VX ? b.vx
                       : g_sortBy == BY_VY ? b.vy : b.flag;
        if (x != y) return g_sortLowFirst ? x < y : x > y;
    }
    return a.frame != b.frame ? a.frame < b.frame : a.flag != b.flag ? a.flag < b.flag : a.route < b.route;
}

struct Rows {
    size_t cap = SIZE_MAX;
    bool evicted = false;
    double matched = 0;
    std::map<std::string, Match> byRoute;
    struct Ord {
        bool operator()(const Match* a, const Match* b) const { return Before(*a, *b); }
    };
    std::set<const Match*, Ord> order;

    void Add(Match&& m, bool count = true) {
        if (count) matched += m.count;
        auto it = byRoute.find(m.route);
        if (it != byRoute.end()) {
            it->second.count += m.count;
            return;
        }
        if (byRoute.size() >= cap && !order.empty() && !Before(m, **order.rbegin())) {
            evicted = true;
            return;
        }
        const std::string key = m.route;
        auto res = byRoute.emplace(key, std::move(m));
        order.insert(&res.first->second);
        if (byRoute.size() > cap) {
            const std::string worst = (*std::prev(order.end()))->route;
            order.erase(std::prev(order.end()));
            byRoute.erase(worst);
            evicted = true;
        }
    }
};

struct Best {
    static constexpr size_t K = 10;
    std::mutex m;
    std::vector<Match> board;

    void Offer(const Match& x) {
        for (const Match& b : board)
            if (b.route == x.route) return;
        if (board.size() >= K && !Before(x, board.back())) return;
        board.insert(std::upper_bound(board.begin(), board.end(), x, Before), x);
        if (board.size() > K) board.pop_back();
    }
};

std::string RowText(const Match& m, bool withCount, bool ownLine = false) {
    char cnt[16] = "", head[160];
    if (withCount) std::snprintf(cnt, sizeof cnt, "%9.0f", m.count);
    if (m.alive)
        std::snprintf(head, sizeof head, "%5d %10.4f %10.4f %10.4f %10.4f %6.0f %9s  ", m.frame, m.x, m.y, m.vx, m.vy, m.flag, cnt);
    else
        std::snprintf(head, sizeof head, "%5d       dead                                  %6.0f %9s  ", m.frame, m.flag, cnt);
    if (!ownLine) return head + m.route + "\n";
    std::string s = head;
    s.erase(s.find_last_not_of(' ') + 1);
    return s + "\n      " + m.route + "\n";
}


struct SharedPrefixes;

struct Force {
    std::vector<Segment> segs;
    std::vector<Cond> conds;
    std::vector<double> rest;
    int pad = 0;
    size_t split = 0;
    bool idleUp = false, rawStates = false, sampling = false;
    double tol = 1e-10;
    size_t rowCap = SIZE_MAX;
    int mergeEvery = 32;
    Seen* seen = nullptr;
    Report* report = nullptr;
    Best* best = nullptr;
    const std::atomic<uint64_t>* played = nullptr;
    const Sim* keyBase = nullptr;  // canonical (or raw) level start
    size_t checkpoints = 0;  // per thread, for prefix reuse
    uint64_t chunks = 1;  // option ranges per task
    size_t lead = 0;  // --stochastic: leading parts kept
    uint64_t leadStates = 0;
    SharedPrefixes* prefixes = nullptr;  // shared --stochastic prefixes

    // Random samples can only merge once few continuations are left.
    bool WorthClaim(size_t nextSeg) const { return !sampling || rest[nextSeg] <= (double)played->load(std::memory_order_relaxed) + 1; }
    bool MidClaims(size_t seg) const { return seen && mergeEvery > 0 && WorthClaim(seg + 1); }
};

struct State {
    Sim sim;
    int met[MAX_CONDS];
    Seq played;
};

void CopyState(State& dst, const State& src) {
    CopySim(dst.sim, src.sim);
    std::memcpy(dst.met, src.met, sizeof dst.met);
    dst.played = src.played;
}

// A state as its 64-byte blocks that differ from the level start.
struct PackedState {
    struct Run { uint32_t a, b; };
    Span live[5];
    size_t liveCount = 0, runCount = 0, playedSize = 0;
    int met[MAX_CONDS];
    std::unique_ptr<Run[]> runs;
    std::unique_ptr<uint8_t[]> bytes;  // runs, then played inputs

    void Restore(State& dst, const Sim& base) const {
        Span dest[5], both[10];
        const size_t destCount = LiveSpans(dst.sim, dst.sim, dest);
        size_t i = 0, j = 0, count = 0;
        while (i < liveCount || j < destCount) {
            const Span s = j == destCount || (i < liveCount && live[i].a <= dest[j].a) ? live[i++] : dest[j++];
            if (count && s.a <= both[count - 1].b) both[count - 1].b = std::max(both[count - 1].b, s.b);
            else both[count++] = s;
        }
        auto* out = reinterpret_cast<uint8_t*>(&dst.sim);
        const auto* initial = reinterpret_cast<const uint8_t*>(&base);
        for (i = 0; i < count; ++i) std::memcpy(out + both[i].a, initial + both[i].a, both[i].b - both[i].a);
        size_t at = 0, span = 0;
        for (i = 0; i < runCount; ++i) {
            const Run r = runs[i];
            while (span < count && both[span].b <= r.a) ++span;
            for (j = span; j < count && both[j].a < r.b; ++j) {
                const size_t a = std::max<size_t>(r.a, both[j].a), b = std::min<size_t>(r.b, both[j].b);
                if (a < b) std::memcpy(out + a, bytes.get() + at + a - r.a, b - a);
            }
            at += r.b - r.a;
        }
        std::memcpy(dst.met, met, sizeof met);
        dst.played.assign(bytes.get() + at, bytes.get() + at + playedSize);
    }
};

struct SharedPrefixes {
    using Id = std::pair<uint64_t, uint64_t>;  // earlier choices, input bits
    static constexpr size_t depths[3] = {8, 12, 16};
    static constexpr size_t BLOCK = 64;
    struct alignas(64) Shard {
        std::mutex mutex;
        std::map<Id, std::unique_ptr<PackedState>> entries;
    } shards[64];
    const Sim& base;
    const size_t capacity;
    std::mutex budgetMutex;
    size_t used = 0, deeperUsed = 0, deepestUsed = 0, entries = 0;
    size_t entriesByDepth[3] = {};
    std::atomic<bool> closed[3]{};

    explicit SharedPrefixes(const Sim& initial, size_t budget)
        : base(initial), capacity(budget > sizeof(SharedPrefixes) ? budget - sizeof(SharedPrefixes) : 0) {}

    bool Supports(size_t depth) const { return depth < 16 || capacity >= 128 * 1024 * 1024; }

    Shard& Bucket(const Id& key) {
        uint64_t x = key.first ^ (key.second + 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return shards[(x ^ (x >> 31)) & 63];
    }
    const PackedState* Find(const Id& key) {
        Shard& shard = Bucket(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto it = shard.entries.find(key);
        return it == shard.entries.end() ? nullptr : it->second.get();
    }
    void Release(size_t cost, bool deeper, bool deepest) {
        std::lock_guard<std::mutex> lock(budgetMutex);
        used -= cost;
        if (deeper) deeperUsed -= cost;
        if (deepest) deepestUsed -= cost;
    }
    void Insert(const Id& key, const State& state, size_t depth) {
        const size_t tier = depth == depths[2] ? 2 : depth == depths[1] ? 1 : 0;
        if (closed[tier].load(std::memory_order_relaxed)) return;
        if (Find(key)) return;
        PackedState::Run plan[(sizeof(Sim) + BLOCK - 1) / BLOCK];
        size_t runCount = 0, dataSize = 0;
        const auto* src = reinterpret_cast<const uint8_t*>(&state.sim);
        const auto* initial = reinterpret_cast<const uint8_t*>(&base);
        for (size_t at = 0; at < sizeof(Sim); at += BLOCK) {
            const size_t end = std::min(at + BLOCK, sizeof(Sim));
            if (std::memcmp(src + at, initial + at, end - at) == 0) continue;
            if (runCount && plan[runCount - 1].b == at) plan[runCount - 1].b = (uint32_t)end;
            else plan[runCount++] = {(uint32_t)at, (uint32_t)end};
            dataSize += end - at;
        }
        const size_t cost = sizeof(PackedState) + 160 + runCount * sizeof(PackedState::Run) + dataSize + state.played.size();
        const bool deeper = depth > depths[0], deepest = depth == depths[2];
        {
            std::lock_guard<std::mutex> lock(budgetMutex);
            // Half the pool for the shallow tiers, an eighth for depth 8.
            if (cost > capacity - used || (deeper && cost > capacity - capacity / 8 - deeperUsed) ||
                (deepest && (!Supports(depth) || cost > capacity / 2 - deepestUsed))) {
                closed[tier].store(true, std::memory_order_relaxed);
                return;
            }
            used += cost;
            if (deeper) deeperUsed += cost;
            if (deepest) deepestUsed += cost;
        }
        try {
            auto packed = std::make_unique<PackedState>();
            packed->liveCount = LiveSpans(state.sim, state.sim, packed->live);
            packed->runCount = runCount;
            packed->playedSize = state.played.size();
            std::memcpy(packed->met, state.met, sizeof state.met);
            packed->runs.reset(new PackedState::Run[runCount]);
            std::memcpy(packed->runs.get(), plan, runCount * sizeof plan[0]);
            packed->bytes.reset(new uint8_t[dataSize + state.played.size()]);
            size_t at = 0;
            for (size_t i = 0; i < runCount; ++i) {
                const auto r = plan[i];
                std::memcpy(packed->bytes.get() + at, src + r.a, r.b - r.a);
                at += r.b - r.a;
            }
            if (!state.played.empty()) std::memcpy(packed->bytes.get() + at, state.played.data(), state.played.size());
            Shard& shard = Bucket(key);
            std::lock_guard<std::mutex> lock(shard.mutex);
            // Entries stay until the workers join.
            if (shard.entries.emplace(key, std::move(packed)).second) {
                std::lock_guard<std::mutex> budgetLock(budgetMutex);
                ++entries;
                ++entriesByDepth[tier];
            } else Release(cost, deeper, deepest);
        } catch (...) {
            Release(cost, deeper, deepest);
            throw;
        }
    }
};

enum Status { GOING, MATCH, FAIL, MERGED };

// The previous option's checkpoints, state keys at merge points and fail point.
struct Resume {
    std::vector<std::unique_ptr<State>> cp;
    std::vector<size_t> at;
    std::vector<Key> key;
    size_t depth = 0, failAt = 0;
    Seq prev;
};

constexpr size_t MIN_SKIP = 2;  // copying a checkpoint costs about two frames of play

size_t SharedPrefix(const Seq& a, const Seq& b) {
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

struct Walker {
    const Force& sc;
    std::vector<std::unique_ptr<State>> pool;
    std::vector<Resume> resume;
    Rows rows;
    std::unique_ptr<Sim> canon = std::make_unique<Sim>();
    std::vector<Seq> buf, look;
    Scratch scratch;
    double merged = 0;
    size_t cpLeft, stopAt = 0;
    Status ended = GOING;
    std::vector<std::unique_ptr<State>> lead;
    std::vector<uint8_t> leadAt;
    uint64_t sampleLead = 0;

    explicit Walker(const Force& s) : sc(s), resume(s.segs.size()), cpLeft(s.checkpoints) {
        for (size_t i = 0; i <= s.segs.size(); ++i) pool.push_back(std::make_unique<State>());
        buf.resize(s.segs.size() + 1);
        look.resize(2 * s.segs.size());
        rows.cap = s.rowCap;
    }

    const Sim& Canon(const State& st) {
        if (sc.rawStates) return st.sim;
        Canonicalize(st.sim, *canon, sc.report != nullptr);
        return *canon;
    }

    bool Claim(State& st, size_t seg, uint64_t rest = 0, Key* stateKey = nullptr) {
        if (!sc.seen) return true;
        const Sim& c = Canon(st);
        const Key s = StateKey(c, *sc.keyBase, st.met, sc.conds.size(), sc.tol);
        if (stateKey) *stateKey = s;
        const uint64_t at = Place(seg, rest);
        const Key k = PlaceKey(s, at);
        const bool fresh = sc.seen->Claim(k);
        if (fresh && sc.report) sc.report->Check(c, k, CoarseKey(c, at, st.met, sc.conds.size()), sc.tol);
        return fresh;
    }

    Status Step(State& st, uint8_t in) {
        Sim& s = st.sim;
        if (!s.playerAlive && s.frameCount >= s.deathFrame + RESPAWN_LAST_PAUSE_FRAMES) return FAIL;
        const int f = s.frameCount + 1;
        const bool alive = s.playerAlive;
        s.Tick(in);
        st.played.push_back(!sc.idleUp && alive && s.upIdle ? (uint8_t)(in & ~2) : in);
        const bool won = s.isTimeStop && DeathWarpFinishValid(s.deathFrame, s.winFrame);
        bool all = true, deadHope = false;
        for (size_t j = 0; j < sc.conds.size(); ++j) {
            if (st.met[j]) continue;
            const Cond& c = sc.conds[j];
            if (f >= c.lo && f <= c.hi && Holds(c, s, won)) {
                st.met[j] = f;
                continue;
            }
            if (f >= c.hi) return FAIL;
            all = false;
            deadHope |= CanHoldDead(c);
        }
        if (all) return MATCH;
        if (s.isTimeStop || (!s.playerAlive && !deadHope)) return FAIL;
        return GOING;
    }

    const Seq& Option(size_t seg, uint64_t k, Seq& out) {
        const Node& n = *sc.segs[seg].node;
        if (n.stored) return n.list[(size_t)k];
        n.Get(k, out, scratch);
        return out;
    }
    const Seq& Option(size_t seg, uint64_t k) { return Option(seg, k, buf[seg]); }

    static uint64_t RestOf(const Seq& in, size_t c, uint64_t later) {
        Hasher h(0x6A09E667F3BCC909ULL ^ later);
        for (size_t t = c; t < in.size(); ++t) h.Mix(in[t]);
        h.Mix(in.size() - c + 1);
        return h.Done().a | 1;
    }

    void Push(Resume& R, const State& st, size_t at) {
        if (R.depth == R.cp.size()) {
            if (cpLeft == 0) {
                if (R.depth == 0) return;
                --R.depth;  // no memory for another: replace the latest
            } else {
                R.cp.push_back(std::make_unique<State>()), R.at.push_back(0);
                --cpLeft;
            }
        }
        CopyState(*R.cp[R.depth], st);
        R.at[R.depth++] = at;
    }

    // Plays an option from input `from`; with --hardfilter also merges every mergeEvery frames.
    Status Play(State& st, const Seq& in, size_t seg, uint64_t later, size_t from = 0, Resume* keep = nullptr, size_t mark = 0) {
        const bool check = sc.MidClaims(seg);
        const size_t every = (size_t)sc.mergeEvery;
        // Shared prefixes only where no merge check falls inside them.
        SharedPrefixes::Id prefixKey[3];
        size_t cacheCount = 0;
        if (sc.prefixes && sc.sampling && seg == sc.lead && from == 0) {
            uint64_t inputs = 0;
            size_t at = 0;
            for (size_t depth : SharedPrefixes::depths) {
                if (depth >= in.size() || (check && every <= depth) || !sc.prefixes->Supports(depth)) break;
                while (at < depth) inputs |= (uint64_t)in[at] << (3 * at), ++at;
                prefixKey[cacheCount++] = {sampleLead, inputs | (1ULL << (3 * depth))};
            }
            for (size_t i = cacheCount; i > 0; --i) {
                if (const PackedState* found = sc.prefixes->Find(prefixKey[i - 1])) {
                    found->Restore(st, sc.prefixes->base);
                    from = SharedPrefixes::depths[i - 1];
                    break;
                }
            }
        }
        for (size_t j = from; j < in.size(); ++j) {
            const Status r = Step(st, in[j]);
            stopAt = j + 1;
            if (r != GOING) return r;
            for (size_t i = 0; i < cacheCount; ++i)
                if (j + 1 == SharedPrefixes::depths[i]) sc.prefixes->Insert(prefixKey[i], st, j + 1);
            if (keep && j + 1 == mark) Push(*keep, st, mark);
            if (check && j + 1 < in.size() && (j + 1) % every == 0) {
                Key* slot = nullptr;
                if (keep) {
                    if (keep->key.size() < (j + 1) / every) keep->key.resize((j + 1) / every);
                    slot = &keep->key[(j + 1) / every - 1];
                }
                if (!Claim(st, seg, RestOf(in, j + 1, later), slot)) return MERGED;
            }
        }
        return GOING;
    }

    void Record(State& st, double count) {
        Match m;
        Sim& s = st.sim;
        for (size_t j = 0; j < sc.conds.size(); ++j) m.frame = std::max(m.frame, st.met[j]);
        m.alive = s.playerAlive;
        if (m.alive) {
            m.x = Value(s, Q_X), m.y = Value(s, Q_Y), m.vx = Value(s, Q_VX), m.vy = Value(s, Q_VY);
        }
        m.flag = Value(s, Q_FLAG);
        m.count = count;
        m.route = EncodeInputsRLE(st.played);
        if (sc.best) {
            std::lock_guard<std::mutex> g(sc.best->m);
            sc.best->Offer(m);
        }
        rows.Add(std::move(m));
    }

    // Plays options lo..hi-1 of part `seg` (all by default) and everything after them.
    void Walk(State& st, size_t seg, uint64_t lo = 0, uint64_t hi = UINT64_MAX) {
        if (seg == sc.segs.size()) {
            for (int k = 0; k < sc.pad; ++k) {
                const Status r = Step(st, IN_NONE);
                if (r == MATCH) Record(st, 1);
                if (r != GOING) return;
            }
            return;
        }
        // Resume from the longest prefix shared with the previous option; skipped merge checks use stored keys.
        hi = std::min(hi, sc.segs[seg].size());
        if (lo >= hi) return;
        Resume& R = resume[seg];
        const bool reuse = hi - lo > 1 && !sc.report;
        const bool check = sc.MidClaims(seg);
        const size_t every = (size_t)sc.mergeEvery;
        R.depth = R.failAt = 0;
        const Seq* opt = &Option(seg, lo, look[2 * seg + (lo & 1)]);
        for (uint64_t k = lo; k < hi && !g_stop.load(std::memory_order_relaxed); ++k) {
            const Seq* next = k + 1 < hi ? &Option(seg, k + 1, look[2 * seg + ((k + 1) & 1)]) : nullptr;
            size_t from = 0, fail = 0, mark = 0;
            if (reuse) {
                if (k > lo) {
                    const size_t shared = SharedPrefix(R.prev, *opt);
                    while (R.depth && R.at[R.depth - 1] > shared) --R.depth;
                    if (R.depth) from = R.at[R.depth - 1];
                    if (R.failAt && shared >= R.failAt) fail = R.failAt;
                }
                if (next) {
                    mark = SharedPrefix(*opt, *next);
                    if (mark < from + MIN_SKIP || (check && mark == opt->size() && mark % every == 0)) mark = 0;
                }
            }
            Status r = GOING;
            const size_t upto = fail ? fail - 1 : from;
            if (check)
                for (size_t c = every; c <= upto && c < opt->size() && r == GOING; c += every)
                    if (!sc.seen->Claim(PlaceKey(R.key[c / every - 1], Place(seg, RestOf(*opt, c, 0))))) r = MERGED;
            State* cur = &st;
            if (r == GOING && fail) r = FAIL;
            else if (r == GOING) {
                if (from > 0) {
                    CopyState(*pool[seg], *R.cp[R.depth - 1]);
                    cur = pool[seg].get();
                } else if (k + 1 < hi) {
                    CopyState(*pool[seg], st);
                    cur = pool[seg].get();
                }
                r = Play(*cur, *opt, seg, 0, from, reuse ? &R : nullptr, mark);
                R.failAt = r == FAIL ? stopAt : 0;
            } else if (!fail) R.failAt = 0;
            if (reuse) R.prev = *opt;
            if (r == MATCH) Record(*cur, sc.rest[seg + 1]);
            else if (r == MERGED) merged += sc.rest[seg + 1];
            else if (r == GOING) {
                if (Claim(*cur, seg + 1)) Walk(*cur, seg + 1);
                else merged += sc.rest[seg + 1];
            }
            opt = next;
        }
    }

    // weight: strings a match or merge stands for; tail: hash of what is still to play.
    bool PlayPrefix(State& st, const std::vector<uint64_t>& digit, size_t from, size_t upto, double weight, uint64_t tail = 0) {
        for (size_t i = from; i < upto; ++i) {
            Hasher todo(0x51ED270B27E84A3DULL);
            for (size_t j = i + 1; j < digit.size(); ++j) todo.Mix(digit[j] + 1);
            if (tail) todo.Mix(tail);
            const uint64_t later = i + 1 < digit.size() || tail ? todo.Done().a : 0;
            const Status r = Play(st, Option(i, digit[i]), i, later);
            ended = r;
            if (r == MATCH) Record(st, weight);
            if (r == MERGED) merged += weight;
            if (r != GOING) return false;
            if (sc.WorthClaim(i + 1) && !Claim(st, i + 1, later)) {
                merged += weight, ended = MERGED;
                return false;
            }
        }
        return true;
    }

    // Task t: one choice for each part before part s = split-1, and a range of the options of s.
    void Task(uint64_t t, State& st) {
        if (sc.split == 0) return Walk(st, 0);
        const size_t s = sc.split - 1;
        const uint64_t m = sc.segs[s].size(), c = t % sc.chunks;
        const uint64_t lo = c * (m / sc.chunks) + std::min(c, m % sc.chunks);
        const uint64_t hi = lo + m / sc.chunks + (c < m % sc.chunks ? 1 : 0);
        t /= sc.chunks;
        std::vector<uint64_t> digit(s);
        for (size_t i = s; i-- > 0;) {
            digit[i] = t % sc.segs[i].size();
            t /= sc.segs[i].size();
        }
        Hasher range(0x2545F4914F6CDD1DULL);
        range.Mix(lo), range.Mix(hi);
        if (PlayPrefix(st, digit, 0, s, (double)(hi - lo) * sc.rest[s + 1], range.Done().a | 1)) Walk(st, s, lo, hi);
    }

    // leadAt: 0 unknown, 1 kept, 2 fails, 3 played each time.
    void Sample(const std::vector<uint64_t>& digit, State& st, const State& base) {
        size_t from = 0;
        sampleLead = 0;
        for (size_t i = 0; i < sc.lead; ++i) sampleLead = sampleLead * sc.segs[i].size() + digit[i];
        if (sc.lead && (!sc.seen || !sc.WorthClaim(sc.lead))) {
            const uint64_t idx = sampleLead;
            if (lead.empty()) lead.resize((size_t)sc.leadStates), leadAt.assign((size_t)sc.leadStates, 0);
            uint8_t& at = leadAt[(size_t)idx];
            if (at == 2) return;
            if (at == 1) CopyState(st, *lead[(size_t)idx]), from = sc.lead;
            else if (at == 0) {
                CopyState(st, base);
                if (!PlayPrefix(st, digit, 0, sc.lead, 1)) {
                    at = ended == FAIL ? 2 : 3;
                    return;
                }
                lead[(size_t)idx] = std::make_unique<State>(st), at = 1, from = sc.lead;
            } else CopyState(st, base);
        } else CopyState(st, base);
        if (!PlayPrefix(st, digit, from, digit.size(), 1)) return;
        for (int k = 0; k < sc.pad; ++k) {
            const Status r = Step(st, IN_NONE);
            if (r == MATCH) Record(st, 1);
            if (r != GOING) return;
        }
    }
};

// A keyed bijection of [0, n): an 8-round Feistel network on the next even number of bits, cycle-walked into range.
struct Shuffle {
    uint64_t n = 1, mask = 0, keys[8] = {};
    int half = 1;
    Shuffle(uint64_t n_, uint64_t seed) : n(n_) {
        int bits = 2;
        while (bits < 62 && (1ULL << bits) < n) bits += 2;
        half = bits / 2;
        mask = (1ULL << half) - 1;
        uint64_t z = seed;
        for (uint64_t& k : keys) {
            z += 0x9E3779B97F4A7C15ULL;
            uint64_t x = z;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            k = x ^ (x >> 31);
        }
    }
    uint64_t Enc(uint64_t x) const {
        uint64_t L = x >> half, R = x & mask;
        for (const uint64_t k : keys) {
            const uint64_t t = L ^ (Avalanche(R ^ k) & mask);
            L = R, R = t;
        }
        return (L << half) | R;
    }
    uint64_t operator()(uint64_t c) const {
        uint64_t y = Enc(c);
        while (y >= n) y = Enc(y);
        return y;
    }
};

}  // namespace

static int Smartforce(int argc, char** argv) {
    int level = 0, checkpoint = 0, top = 30, threads = (int)std::max(1u, std::thread::hardware_concurrency());
    bool gless = false, list = false, names = false, hardfilter = false, mergeReport = false, stochastic = false, seedGiven = false;
    double maxMemGB = 4, timeLimit = 0, updateEvery = 60;
    bool save = false;
    std::string saveName;
    uint64_t maxSamples = 0, seed = 0;
    std::string inputs, report = "win";
    Force sc;
    for (int i = 2; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--level")) level = std::atoi(next());
        else if (!std::strcmp(argv[i], "--inputs")) inputs = next();
        else if (!std::strcmp(argv[i], "--report")) report = next();
        else if (!std::strcmp(argv[i], "--checkpoint")) checkpoint = std::atoi(next());
        else if (!std::strcmp(argv[i], "--pad")) sc.pad = std::max(0, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--threads")) threads = std::max(1, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--top")) top = std::max(0, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--gless")) gless = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--names")) names = true;
        else if (!std::strcmp(argv[i], "--idle-up")) sc.idleUp = true;
        else if (!std::strcmp(argv[i], "--hardfilter")) hardfilter = true;
        else if (!std::strcmp(argv[i], "--tolerance")) sc.tol = std::max(0.0, std::atof(next()));
        else if (!std::strcmp(argv[i], "--merge-report")) hardfilter = mergeReport = true;
        else if (!std::strcmp(argv[i], "--raw-states")) sc.rawStates = true;
        else if (!std::strcmp(argv[i], "--maxmem")) maxMemGB = std::atof(next());
        else if (!std::strcmp(argv[i], "--stochastic")) stochastic = true;
        else if (!std::strcmp(argv[i], "--time")) timeLimit = std::max(0.0, std::atof(next()));
        else if (!std::strcmp(argv[i], "--samples")) maxSamples = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--update")) updateEvery = std::max(0.0, std::atof(next()));
        else if (!std::strcmp(argv[i], "--merge-every")) sc.mergeEvery = std::max(0, std::atoi(next()));
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(next(), nullptr, 10), seedGiven = true;
        else if (!std::strcmp(argv[i], "--save")) save = true, saveName = i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) ? argv[++i] : "";
        else if (!std::strcmp(argv[i], "--sortby")) {
            std::string q = next();
            g_sortLowFirst = !q.empty() && q[0] == '-';
            if (g_sortLowFirst) q.erase(0, 1);
            const char* names[] = {"frame", "px", "py", "vx", "vy", "flag"};
            g_sortBy = BY_DEFAULT;
            for (int k = 0; k < 6; ++k)
                if (q == names[k]) g_sortBy = (SortBy)(BY_FRAME + k);
            if (g_sortBy == BY_DEFAULT) {
                std::fprintf(stderr, "--sortby takes frame, px, py, vx, vy or flag (highest first; -px etc.: lowest first)\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!(maxMemGB > 0) || !std::isfinite(maxMemGB)) {
        std::fprintf(stderr, "--maxmem needs a positive number of gigabytes\n");
        return 2;
    }
    const double ram = PhysicalMemoryBytes();
    if (ram > 0 && maxMemGB * 1e9 > ram) {
        std::fprintf(stderr, "--maxmem %g GB is more than this PC's memory (%.1f GB)\n", maxMemGB, ram / 1e9);
        return 2;
    }
    if (!level || (inputs.empty() && !names)) {
        std::fprintf(stderr, "usage: rbsim smartforce --level N --inputs PATTERN [--report CONDITIONS] [--checkpoint C] [--gless] [--pad N]\n"
            "                        [--threads N] [--top N] [--list] [--names] [--idle-up] [--hardfilter [--tolerance T]]\n"
            "                        [--merge-report] [--maxmem GB] [--stochastic [--time SEC] [--samples N] [--seed S]\n"
            "                        [--update SEC]] [--merge-every N] [--sortby [-]Q] [--save [FILE]]\n"
            "pattern:\n"
            "  d18 n3      runs as in a TAS string (a letter without a count is 1 frame)\n"
            "  d4..7       every count from 4 to 7 (d0..3: 0 to 3 frames)\n"
            "  (A|B|C)     alternatives, each a sub-pattern\n"
            "  (...)=6     only the group's expansions that are 6 frames long (=4..6: 4 to 6)\n"
            "  <BASE:a3>   every way to overwrite 3 single frames of BASE (any sub-pattern) with a: <n20:a3>, <d18e3:n2>\n"
            "  <BASE+a3>   the same, but a is added to the keys held on those frames: <d20+a1> puts one S (left+right)\n"
            "  <BASE:a1..3>, <BASE+a0..2>   every tap count in the range, for either form (0: BASE unchanged)\n"
            "  <BASE:a1,w1>  several tap kinds on different frames (never on the same one): one a and one w;\n"
            "              each kind takes its own count or range: <n20:a1..2,w0..1>, <d20+a1,w2>\n"
            "  e.g. \"d18(e1..6d0..5)=6<n4:a1>(d4..7e7..10)=14\" = 6 x 4 x 4 = 96 strings\n"
            "  Strings come shortest first, then in written order. Every part of the pattern is expanded in memory (repeats\n"
            "  removed) if it fits in --maxmem GB (default 4); a part that does not is generated one string at a time\n"
            "  instead (no limit, repeats kept; --hardfilter merges them).\n"
            "report: comma-separated conditions, all must hold; each may have a frame window @F, @A..B, @..B, @A..\n"
            "  win  dead  alive  ground (a jump probe touches)  contact  contact=NAME (--names lists them)\n"
            "  Q<V  Q<=V  Q>V  Q>=V  Q=V   Q: px py vx vy vr (as the mod's HUD and /set: metres, m/s), pr (rotation in\n"
            "                              degrees as the HUD shows it: fmod(angle, 360), negative when turned backwards),\n"
            "                              flag (distance from the ball's box to the flag's in twips, <= 0: touching)\n"
            "  e.g. --report \"contact=kickBall@40..60, px>=12.5@80\" (default: win)\n"
            "frames are the game's (the flag frame is the one on the game's timer). A string is decided on the frame all conditions hold;\n"
            "the rest of it is not played. --pad N: N extra n frames after each string. --list: print the strings only.\n"
            "Rows (and the best kept) are ordered by frame, then flag; --sortby Q (frame px py vx vy flag) orders them by\n"
            "that column, highest first (--sortby -Q: lowest first; dead rows last for px py vx vy), ties by frame, flag.\n"
            "--save [FILE]: at the end (also after Ctrl+C) write all result rows, not just --top, to FILE after a line with\n"
            "the command; by default smartforce-L<level>-<date>-<time>.txt (FILE ending in / or \\: in that folder;\n"
            "folders are created). An existing file is never overwritten: -2, -3, ... is added to the name.\n"
            "Up held on a frame where it does nothing (ball awake, no jump: no contact or no foot probe, no lift: vy >= 0) is\n"
            "written as the same keys without Up (the game plays both identically). --idle-up keeps the strings as written.\n"
            "--hardfilter: strings in the same game state at the same point of the pattern, with the same inputs still to\n"
            "play, are merged: only the first is played on. States are compared without data the game never reads again\n"
            "(freed contacts, stale manifold points, caches); doubles count as equal within --tolerance T (default 1e-10;\n"
            "0: bit-exact only). With a tolerance the merge is a heuristic: a 1e-10 difference can grow. --merge-report: also\n"
            "list what still differs between states whose bodies agree to 1e-7 but that were not merged; --raw-states\n"
            "compares the full state instead. States are compared between the parts of the pattern and every\n"
            "--merge-every N frames inside a part (default 32; 0: only between parts).\n"
            "--stochastic: play the strings in a uniformly random order without repeats and without storing them (a keyed\n"
            "bijection of all string numbers), until all are played or Ctrl+C (or --time SEC, --samples N); every\n"
            "--update SEC (default 60, 0: off) it prints how many strings it has played and how fast, then the 10 best\n"
            "so far. --seed S repeats a run. Ctrl+C stops any search and prints what was found.\n");
        return 2;
    }
    if (level < 1 || level > NUM_LEVELS || !GetLevelScript(level).implemented) {
        std::fprintf(stderr, "level %d is not implemented\n", level);
        return 2;
    }
    LevelTemplate tpl(level);
    if (checkpoint < 0 || checkpoint >= tpl.CheckpointCount()) {
        std::fprintf(stderr, "level %d has checkpoints 0 to %d (0 = the level start)\n", level, tpl.CheckpointCount() - 1);
        return 2;
    }
    WarnIfUnverified(level);
    // Created now so a bad path fails early; rows are written at the end.
    struct CloseSave {
        FILE* f = nullptr;
        ~CloseSave() {
            if (f) std::fclose(f);
        }
    } saveFile;
    if (save) {
        const std::time_t now = std::time(nullptr);
        char stamp[32], when[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", std::localtime(&now));
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        const std::string automatic = "smartforce-L" + std::to_string(level) + "-" + stamp + ".txt";
        if (saveName.empty()) saveName = automatic;
        else if (saveName.back() == '/' || saveName.back() == '\\') saveName += automatic;
        saveName = FreeName(saveName);
        std::error_code ec;
        const std::filesystem::path folder = std::filesystem::path(saveName).parent_path();
        if (!folder.empty()) std::filesystem::create_directories(folder, ec);
        if (!(saveFile.f = std::fopen(saveName.c_str(), "w"))) {
            std::fprintf(stderr, "--save: cannot write %s\n", saveName.c_str());
            return 2;
        }
        std::string cmd = "rbsim";
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            cmd += a.find_first_of(" \t\"<>|&()") == std::string::npos && !a.empty() ? " " + a : " \"" + a + "\"";
        }
        std::fprintf(saveFile.f, "# %s\n# started %s\n", cmd.c_str(), when);
        std::fflush(saveFile.f);
        std::fprintf(stderr, "the results will be saved to %s\n", saveName.c_str());
    }
    auto base = std::make_unique<State>();
    base->sim.Load(&tpl, checkpoint);
    base->sim.gless = gless;
    std::memset(base->met, 0, sizeof base->met);
    if (names) {
        std::vector<std::string> n(tpl.bodyNames.begin(), tpl.bodyNames.end());
        n.push_back("ground");
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
        for (const std::string& s : n) std::printf("%s\n", s.c_str());
        return 0;
    }

    const double budget = maxMemGB * 1e9;
    std::string err;
    double storedBytes = 0;
    sc.segs = ParsePattern(inputs, budget, storedBytes, err);
    if (!err.empty()) {
        std::fprintf(stderr, "pattern: %s\n", err.c_str());
        return 2;
    }
    sc.rest.assign(sc.segs.size() + 1, 1);
    for (size_t i = sc.segs.size(); i-- > 0;) sc.rest[i] = sc.rest[i + 1] * (double)sc.segs[i].size();
    std::string sizes;
    bool anyGenerated = false;
    for (const Segment& o : sc.segs)
        if (o.size() > 1) {
            sizes += (sizes.empty() ? "" : " x ") + std::to_string(o.size()) + (o.generated() ? "*" : "");
            anyGenerated |= o.generated();
        }
    const std::string sizeNote = sizes.empty() ? "" : " (" + sizes + ")";
    uint64_t total = 1;
    bool exactTotal = true;
    for (const Segment& o : sc.segs) {
        if (o.size() > SAT / std::max<uint64_t>(total, 1)) exactTotal = false;
        else total *= o.size();
    }
    if (stochastic && !seedGiven) seed = ((uint64_t)std::random_device{}() << 32) ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    auto digitsOf = [&](uint64_t idx, std::vector<uint64_t>& d, std::mt19937_64* rng) {
        d.resize(sc.segs.size());
        if (rng) {
            for (size_t i = 0; i < sc.segs.size(); ++i) d[i] = std::uniform_int_distribution<uint64_t>(0, sc.segs[i].size() - 1)(*rng);
            return;
        }
        for (size_t i = sc.segs.size(); i-- > 0;) {
            d[i] = idx % sc.segs[i].size();
            idx /= sc.segs[i].size();
        }
    };

    if (list) {
        if (stochastic && top == 0 && !exactTotal) {
            std::fprintf(stderr, "--list --stochastic --top 0 would never end: the pattern has more than 4.6e18 strings\n");
            return 2;
        }
        Say("%.0f strings%s%s\n", sc.rest[0], sizeNote.c_str(), anyGenerated ? ", * = generated, not stored" : "");
        std::vector<uint64_t> d(sc.segs.size(), 0);
        Seq part;
        Scratch scratch;
        const Shuffle shuf(exactTotal ? total : 1, seed);
        std::mt19937_64 rng(seed);
        for (int n = 0; (top == 0 || n < top) && !(exactTotal && (uint64_t)n >= total); ++n) {
            if (stochastic) digitsOf(exactTotal ? shuf((uint64_t)n) : 0, d, exactTotal ? nullptr : &rng);
            Seq s;
            for (size_t i = 0; i < sc.segs.size(); ++i) {
                sc.segs[i].Get(d[i], part, scratch);
                s.insert(s.end(), part.begin(), part.end());
            }
            Say("%s\n", EncodeInputsRLE(s).c_str());
            if (stochastic) continue;
            size_t i = sc.segs.size();
            while (i > 0 && ++d[i - 1] == sc.segs[i - 1].size()) d[--i] = 0;
            if (i == 0) break;
        }
        return 0;
    }

    size_t start = 0;
    while (start <= report.size()) {
        size_t end = report.find(',', start);
        if (end == std::string::npos) end = report.size();
        const std::string tok = Trim(report.substr(start, end - start));
        start = end + 1;
        if (tok.empty()) continue;
        Cond c;
        if (!ParseCond(tok, c, err)) {
            std::fprintf(stderr, "report: %s\n", err.c_str());
            return 2;
        }
        if (c.kind == K_CONTACT && !c.name.empty() && c.name != "ground" &&
            std::find(tpl.bodyNames.begin(), tpl.bodyNames.end(), c.name) == tpl.bodyNames.end()) {
            std::fprintf(stderr, "report: level %d has no body named '%s' (see --names)\n", level, c.name.c_str());
            return 2;
        }
        sc.conds.push_back(c);
    }
    if (sc.conds.empty() || sc.conds.size() > MAX_CONDS) {
        std::fprintf(stderr, "report: 1 to %d conditions\n", MAX_CONDS);
        return 2;
    }

    const double fixedBytes = (double)threads * ((double)(sc.segs.size() + 3) * sizeof(State) + sizeof(Sim)) +
                              (mergeReport ? 260.0 * sizeof(Sim) : 0.0);
    if (storedBytes + fixedBytes > budget) {
        std::fprintf(stderr, "%d threads need about %.2f GB for their game states, more than is left of --maxmem %g: use fewer --threads or a larger --maxmem\n",
                     threads, fixedBytes / 1e9, maxMemGB);
        return 2;
    }
    const double spare = std::max(0.0, budget - storedBytes - fixedBytes);
    std::unique_ptr<Seen> seen;
    std::unique_ptr<Report> mrep;
    std::unique_ptr<Sim> keyBase;
    if (hardfilter) {
        seen = std::make_unique<Seen>(), sc.seen = seen.get(), GetLayout();
        seen->cap = (size_t)std::max(1000.0, spare * 0.6 / SEEN_ENTRY_BYTES);
        if (sc.rawStates) sc.keyBase = &base->sim;
        else keyBase = std::make_unique<Sim>(), Canonicalize(base->sim, *keyBase, true), sc.keyBase = keyBase.get();
    }
    sc.checkpoints = (size_t)std::min(stochastic ? 256.0 : 16.0 * (double)sc.segs.size(), spare * 0.1 / threads / sizeof(State));
    if (mergeReport) mrep = std::make_unique<Report>(), sc.report = mrep.get();
    const double rowShare = std::max(1000.0, spare * 0.3 / ROW_BYTES / threads);
    sc.rowCap = (size_t)(top > 0 ? std::min(rowShare, std::max(10000.0, 4.0 * top)) : rowShare);
    Best best;
    sc.sampling = stochastic;
    if (stochastic && updateEvery > 0) sc.best = &best;
    std::atomic<uint64_t> nextTask{0}, done{0};
    sc.played = &done;
    std::signal(SIGINT, OnInterrupt);

    Say("level %d: %.0f strings%s, report: %s, %d threads\n", level, sc.rest[0], sizeNote.c_str(), report.c_str(), threads);
    Say("memory: %.0f MB of the pattern stored%s, limit %g GB\n", storedBytes / 1e6,
                anyGenerated ? ", * = generated one string at a time" : "", maxMemGB);
    if (stochastic) {
        Say("stochastic: %s, seed %llu, %s\n",
                    exactTotal ? "uniformly random order without repeats" : "independent random choices (too many strings to number)",
                    (unsigned long long)seed,
                    timeLimit > 0 ? [&] { static char b[64]; std::snprintf(b, sizeof b, "time limit %g s", timeLimit); return (const char*)b; }()
                                  : "no time limit");
        std::fflush(stdout);
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::atomic<int> running{threads};
    std::atomic<bool> outOfMemory{false};
    std::atomic<bool> timeUp{false};
    std::vector<std::unique_ptr<Walker>> walkers;
    std::vector<std::thread> pool;
    uint64_t tasks = 1;
    const uint64_t wantTasks = (uint64_t)threads * 32;
    if (!stochastic) {
        while (sc.split < sc.segs.size() && tasks < wantTasks && sc.segs[sc.split].size() <= SAT / tasks)
            tasks *= sc.segs[sc.split++].size();
        if (sc.split > 0) {
            const uint64_t m = sc.segs[sc.split - 1].size(), outer = tasks / m;
            sc.chunks = std::min(m, std::max<uint64_t>(1, (wantTasks + outer - 1) / outer));
            tasks = outer * sc.chunks;
        }
    } else {
        for (uint64_t n = 1; sc.lead + 1 < sc.segs.size() && sc.segs[sc.lead].size() <= sc.checkpoints / n;)
            n *= sc.segs[sc.lead++].size(), sc.leadStates = n;
    }
    std::unique_ptr<SharedPrefixes> prefixes;
    if (stochastic && sc.checkpoints > sc.leadStates) {
        const size_t prefixBudget = (size_t)threads * (sc.checkpoints - (size_t)sc.leadStates) * sizeof(State);
        prefixes = std::make_unique<SharedPrefixes>(base->sim, prefixBudget);
        sc.prefixes = prefixes.get();
    }
    const Shuffle shuf(exactTotal ? total : 1, seed);
    for (int k = 0; k < threads; ++k) walkers.push_back(std::make_unique<Walker>(sc));
    auto work = [&](int k) {
        auto st = std::make_unique<State>();
        Walker& w = *walkers[(size_t)k];
        if (!stochastic) {
            for (uint64_t t; !g_stop && (t = nextTask.fetch_add(1)) < tasks; done.fetch_add(1)) {
                CopyState(*st, *base);
                w.Task(t, *st);
            }
            return;
        }
        std::mt19937_64 rng(seed ^ (0xD1B54A32D192ED03ULL * (uint64_t)(k + 1)));
        std::vector<uint64_t> d;
        for (;;) {
            if (g_stop || timeUp) break;
            const uint64_t c = nextTask.fetch_add(1);
            if ((exactTotal && c >= total) || (maxSamples && c >= maxSamples)) break;
            if (timeLimit > 0 && elapsed() > timeLimit) {
                timeUp = true;
                break;
            }
            digitsOf(exactTotal ? shuf(c) : 0, d, exactTotal ? nullptr : &rng);
            w.Sample(d, *st, *base);
            done.fetch_add(1);
        }
    };
    for (int k = 0; k < threads; ++k)
        pool.emplace_back([&, k]() {
            try {
                work(k);
            } catch (const std::bad_alloc&) {
                outOfMemory = true;
                g_stop = true;
            }
            --running;
        });
    bool shown = false;
    double nextUpdate = updateEvery, lastSec = 0;
    uint64_t lastDone = 0;
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const double sec = elapsed();
        if (stochastic && updateEvery > 0 && sec >= nextUpdate && running.load() > 0) {
            nextUpdate += updateEvery;
            try {
                std::vector<Match> board;
                {
                    std::lock_guard<std::mutex> g(best.m);
                    board = best.board;
                }
                const uint64_t played = done.load();
                char head[256];
                std::snprintf(head, sizeof head, "--- %.0f s: %llu of %.0f strings played (%.3g%%), %.0f/s (%.0f/s since the last update)\n%s\n",
                              sec, (unsigned long long)played, sc.rest[0], 100.0 * (double)played / sc.rest[0], played / sec,
                              (played - lastDone) / std::max(1e-9, sec - lastSec),
                              board.empty() ? "no match yet" : ("the " + std::to_string(board.size()) + " best so far:").c_str());
                lastDone = played, lastSec = sec;
                std::string text = head;
                if (!board.empty()) text += "frame         px         py         vx         vy   flag             inputs\n";
                for (const Match& m : board) text += RowText(m, false);
                PrintText(text);
            } catch (const std::bad_alloc&) {
                outOfMemory = true;
                g_stop = true;
            }
        }
        if (!stochastic && sec > 3) {
            std::fprintf(stderr, "\r  %llu / %llu tasks, %.0f s ", (unsigned long long)done.load(), (unsigned long long)tasks, sec);
            shown = true;
        }
    }
    for (auto& t : pool) t.join();
    if (shown) std::fprintf(stderr, "\n");
    std::signal(SIGINT, SIG_DFL);
    const double sec = elapsed();

    Rows all;
    double sameState = 0;
    bool evicted = false;
    for (auto& w : walkers) {
        all.matched += w->rows.matched;
        evicted |= w->rows.evicted;
        sameState += w->merged;
        for (auto& kv : w->rows.byRoute) all.Add(std::move(kv.second), false);
    }
    std::vector<Match> res;
    for (const Match* m : all.order) res.push_back(*m);
    const size_t distinct = all.byRoute.size();
    const char* atLeast = evicted ? "at least " : "";
    if (stochastic) {
        const uint64_t played = done.load();
        Say("played %llu of %.0f strings (%.3g%%), %.0f match, %s%zu distinct, %.1f s, stopped: %s\n", (unsigned long long)played,
                    sc.rest[0], 100.0 * (double)played / sc.rest[0], all.matched, atLeast, distinct, sec,
                    outOfMemory ? "out of memory" : g_stop ? "Ctrl+C" : timeUp ? "time limit" : (maxSamples && played >= maxSamples) ? "--samples" : "all strings played");
    } else {
        Say("%.0f of %.0f strings match, %s%zu distinct up to the deciding frame, %.1f s%s\n", all.matched, sc.rest[0], atLeast,
                    distinct, sec, outOfMemory ? "" : g_stop ? " (stopped by Ctrl+C: results so far)" : "");
    }
    if (hardfilter)
        Say("%.0f strings merged into an earlier one with the same game state (tolerance %g; not counted above)%s\n", sameState,
                    sc.tol, seen->full ? "; the merge table reached its --maxmem share, later states were not added" : "");
    if (evicted) Say("note: only the best rows were kept (memory share of --maxmem); the strings column may undercount\n");
    if (outOfMemory) Say("stopped early: out of memory (lower --maxmem or --threads); the results above are partial\n");
    if (mrep) mrep->Print();
    if (saveFile.f) {
        if (stochastic) std::fprintf(saveFile.f, "# seed %llu\n", (unsigned long long)seed);
        std::fputs(res.empty() ? "# no string matched\n" : "frame         px         py         vx         vy   flag   strings\n      inputs\n", saveFile.f);
        for (const Match& m : res) std::fputs(RowText(m, true, true).c_str(), saveFile.f);
        const bool ok = !std::ferror(saveFile.f) && !std::fclose(saveFile.f);
        saveFile.f = nullptr;
        if (ok) std::fprintf(stderr, "saved %zu rows to %s\n", res.size(), saveName.c_str());
        else std::fprintf(stderr, "--save: writing %s failed\n", saveName.c_str());
    }
    if (res.empty()) return 1;
    Say("frame         px         py         vx         vy   flag   strings  inputs\n");
    std::string rows;
    for (size_t i = 0; i < res.size() && (top == 0 || (int)i < top); ++i) rows += RowText(res[i], true);
    PrintText(rows);
    if (top && res.size() > (size_t)top) Say("... %s%zu more (--top 0 shows all)\n", atLeast, res.size() - (size_t)top);
    return 0;
}

int CmdSmartforce(int argc, char** argv) {
    try {
        return Smartforce(argc, argv);
    } catch (const std::bad_alloc&) {
        std::fflush(stdout);
        std::fprintf(stderr, "out of memory: lower --maxmem or --threads\n");
        return 3;
    }
}
