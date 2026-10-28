// smartforce.cpp - `rbsim smartforce`: this is basically red ball regex
#include "redball.h"
#include "jumpbrute.h"
#include "search_offset.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

struct JumpSummary {
    size_t position;
    std::string minimum, maximum;
    bool stateDependent = false;
};

struct Parser {
    static constexpr int MAX_DEPTH = 200;
    static constexpr double MAX_WORK = 4e8;
    std::string s;
    size_t i = 0;
    std::string err;
    double budget = 4e9, used = 0, work = 0;
    int depth = 0;
    std::vector<JumpSummary> jumps;
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

// Placements of k presses on L frames whose reverse indices (L - frame) sum to k(L - k) + 1 consecutive values.
uint64_t JumpWays(long L, long lo, long hi) {
    const __int128 a = lo, b = std::min<long>(hi, L);
    if (b < a) return 0;
    const __int128 s1 = (b * (b + 1) - (a - 1) * a) / 2, s2 = (b * (b + 1) * (2 * b + 1) - (a - 1) * a * (2 * a - 1)) / 6;
    const __int128 w = (__int128)L * s1 - s2 + (b - a + 1);
    return w >= (__int128)SAT ? SAT : (uint64_t)w;
}

// Reverse indices are 1..L. The highest allowed press count also has the highest sum.
uint64_t JumpMaxSum(long L, long k) {
    return (uint64_t)k * (uint64_t)(2 * L - k + 1) / 2;
}

uint64_t JumpCap(long L, long hi, long percent) {
    return JumpMaxSum(L, std::min(hi, L)) * (uint64_t)percent / 100;
}

uint64_t JumpFloor(long L, long hi, long percent) {
    return (JumpMaxSum(L, std::min(hi, L)) * (uint64_t)percent + 99) / 100;
}

// The old ordering starts at the highest sum. Skip its prefix above the cap.
uint64_t JumpSkip(long L, long k, uint64_t cap) {
    const uint64_t highest = JumpMaxSum(L, k);
    return highest > cap ? highest - cap : 0;
}

uint64_t RangedJumpWays(long L, long k, uint64_t floor, uint64_t cap) {
    const uint64_t low = std::max(floor, (uint64_t)k * (uint64_t)(k + 1) / 2);
    const uint64_t high = std::min(cap, JumpMaxSum(L, k));
    return low <= high ? high - low + 1 : 0;
}

// The jump brute-forcer's placement (BruteForcer.as) for the sum d below the highest: a block from the first frame,
// one press in between, a block at the end.
template <class F>
void JumpPresses(long L, long k, uint64_t d, F&& press) {
    if (k == 0) return;
    if (k == L) {
        for (long i = 0; i < L; ++i) press(i);
        return;
    }
    const long shift = L - k, end = (long)(d / (uint64_t)shift), mid = (long)(d % (uint64_t)shift), first = k - end - 1;
    for (long i = 0; i < first; ++i) press(i);
    if (end < k) press(first + mid);
    for (long i = L - end; i < L; ++i) press(i);
}

// Strings are ordered by length, then as written; cnt[L] = strings of length L.
struct Node {
    enum Kind { RUN, SEQ, ALT, PLACE, JUMP, REDUCED, BRUTE, LIST } kind = RUN;
    uint8_t code = 0;
    long lo = 0, hi = 0;
    long jumpPercentLo = 0, jumpPercentHi = 100;
    long bruteLo = 1, bruteHi = 1;
    size_t position = 0;
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
        if (kind == ALT || kind == PLACE || kind == JUMP || kind == REDUCED)
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
        } else if (kind == LIST) {
            Options o;
            o.swap(list);
            if (!stored) {
                DedupeKeepOrder(o);
                std::stable_sort(o.begin(), o.end(), [](const Seq& a, const Seq& b) { return a.size() < b.size(); });
            }
            if (filtered) o.erase(std::remove_if(o.begin(), o.end(), [&](const Seq& q) { return (long)q.size() < flo || (long)q.size() > fhi; }), o.end());
            SetList(std::move(o));
        } else if (kind == BRUTE) {
            // Fixed slots give deterministic task splitting and a shuffled domain
            // without repeats. The actual, state-dependent choices occupy the
            // beginning of each (base, duration) slot group; skip its unused tail.
            cnt = kids[0]->cnt;
            const long last = std::min(bruteHi, (long)cnt.size() - 1);
            ways.assign(2, {});
            ways[0].resize(cnt.size());
            ways[1].resize((size_t)last + 2);
            if (!p.Spend((double)last + cnt.size())) return false;
            for (long b = 1; b <= last; ++b)
                ways[1][(size_t)b + 1] = SatAdd(ways[1][(size_t)b], b >= bruteLo ? JumpWays(b - 1, lo, hi) : 0);
            for (size_t L = 0; L < cnt.size(); ++L) {
                ways[0][L] = ways[1][std::min(L, (size_t)last) + 1];
                cnt[L] = SatMul(cnt[L], ways[0][L]);
            }
        } else if (kind == JUMP || kind == REDUCED) {
            cnt = kids[0]->cnt;
            ways.assign(1, std::vector<uint64_t>(cnt.size(), 0));
            for (size_t L = 0; L < cnt.size(); ++L) {
                if (!cnt[L]) continue;
                uint64_t w = 0;
                if (jumpPercentLo == 0 && jumpPercentHi == 100) w = JumpWays((long)L, lo, hi);
                else {
                    const long last = std::min(hi, (long)L);
                    if (!p.Spend((double)std::max(0L, last - lo + 1))) return false;
                    const uint64_t floor = JumpFloor((long)L, hi, jumpPercentLo);
                    const uint64_t cap = JumpCap((long)L, hi, jumpPercentHi);
                    for (long k = lo; k <= last; ++k) w = SatAdd(w, RangedJumpWays((long)L, k, floor, cap));
                }
                cnt[L] = SatMul(cnt[L], ways[0][L] = w);
            }
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
        } else if (kind == JUMP || kind == REDUCED) {
            const uint64_t w = ways[0][(size_t)L];
            const size_t start = out.size();
            kids[0]->GetByLen(L, r / w, out, sc);
            r %= w;
            const uint64_t floor = JumpFloor(L, hi, jumpPercentLo);
            const uint64_t cap = JumpCap(L, hi, jumpPercentHi);
            for (long k = lo; k <= std::min(hi, L); ++k) {
                const uint64_t c = RangedJumpWays(L, k, floor, cap);
                if (r >= c) {
                    r -= c;
                    continue;
                }
                JumpPresses(L, k, r + JumpSkip(L, k, cap), [&](long i) {
                    uint8_t& x = out[start + (size_t)i];
                    x = add ? (uint8_t)(x | code) : code;
                });
                break;
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

    void Append(uint64_t i, Seq& out, Scratch& sc) const {
        if (stored) {
            const Seq& q = list[(size_t)i];
            out.insert(out.end(), q.begin(), q.end());
            return;
        }
        const long L = (long)(std::upper_bound(cum.begin(), cum.end(), i) - cum.begin()) - 1;
        GetByLen(L, i - cum[(size_t)L], out, sc);
    }

    void Get(uint64_t i, Seq& out, Scratch& sc) const {
        out.clear();
        Append(i, out, sc);
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

    bool SummarizeJump(Parser& p) const {
        struct Witness {
            bool found = false;
            long length = 0, presses = 0;
            uint64_t sum = 0;
        } minimum, maximum;
        for (size_t length = 0; length < cnt.size(); ++length) {
            if (!cnt[length]) continue;
            const long L = (long)length, last = std::min(hi, L);
            const uint64_t floor = JumpFloor(L, hi, jumpPercentLo), cap = JumpCap(L, hi, jumpPercentHi);
            if (last < lo || floor > cap) continue;
            // Both extreme sums grow with press count. Locate the feasible counts without enumerating strings.
            long a = lo, b = last + 1;
            while (a < b) {
                const long mid = a + (b - a) / 2;
                if (JumpMaxSum(L, mid) < floor) a = mid + 1;
                else b = mid;
            }
            const long first = a;
            a = lo, b = last + 1;
            while (a < b) {
                const long mid = a + (b - a) / 2;
                if ((uint64_t)mid * (uint64_t)(mid + 1) / 2 <= cap) a = mid + 1;
                else b = mid;
            }
            const long final = a - 1;
            if (first > final) continue;
            const uint64_t low = std::max(floor, (uint64_t)first * (uint64_t)(first + 1) / 2);
            const uint64_t high = std::min(cap, JumpMaxSum(L, final));
            if (!minimum.found || low < minimum.sum) minimum = {true, L, first, low};
            if (!maximum.found || high > maximum.sum) maximum = {true, L, final, high};
        }
        auto route = [&](const Witness& w) {
            if (!w.found) return std::string("none");
            Seq q;
            Scratch scratch;
            kids[0]->GetByLen(w.length, 0, q, scratch);
            JumpPresses(w.length, w.presses, JumpMaxSum(w.length, w.presses) - w.sum, [&](long i) {
                uint8_t& x = q[(size_t)i];
                x = add ? (uint8_t)(x | IN_U) : (uint8_t)IN_U;
            });
            return q.empty() ? std::string("n0") : EncodeInputsRLE(q);
        };
        JumpSummary summary{position, route(minimum), route(maximum)};
        if (!p.Charge(2 * StringBytes(0) + (double)(summary.minimum.size() + summary.maximum.size()))) return false;
        p.jumps.push_back(std::move(summary));
        return true;
    }

    bool Build(Parser& p) {
        for (auto& k : kids)
            if (!k->Build(p)) return false;
        for (const auto& k : kids)
            if (k->kind == BRUTE) {
                p.Fail("a +b block must be a separate part of the pattern, not nested inside another block or alternative");
                return false;
            }
        if (!Count(p)) return false;
        if (kind == BRUTE) {
            p.jumps.push_back({position, {}, {}, true});
            return true; // requires a real incoming Sim; never materialize statically
        }
        // Capture witnesses while the child's lookup tables still exist; materialization may release them.
        if (kind == JUMP && !SummarizeJump(p)) return false;
        if (kind == LIST) {
            Recharge(p);
            if (p.used <= p.budget) return true;
            p.Fail("the results file needs more than --maxmem");
            return false;
        }
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

// Pure candidate arithmetic shared by prepared and on-demand generation.
// The caller supplies the single BruteStart velocity measurement.
template <class F>
uint64_t BruteRows(const Node& n, int frames, double vy, F&& row) {
    const JumpBrute model(frames, vy);
    int last = (int)std::min<long>(n.hi, model.MaxCount());
    while (last >= n.lo && model.Window(last) < last) --last;
    uint64_t count = 0;
    if (last >= n.lo) {
        const uint64_t maximum = model.Maximum(last);
        const uint64_t floor = (maximum * (uint64_t)n.jumpPercentLo + 99) / 100;
        const uint64_t cap = maximum * (uint64_t)n.jumpPercentHi / 100;
        for (int k = (int)n.lo; k <= last; ++k) {
            if (model.Window(k) < k) continue;
            const uint64_t low = std::max(floor, model.Minimum(k)), high = std::min(cap, model.Maximum(k));
            if (low <= high) {
                count += high - low + 1;
                row(k, low, high, count);
            }
        }
    }
    return count;
}

// One group at a time, per worker and pattern part. No game states are retained
// here. Reset whenever the incoming prefix changes; the caller owns the Sim.
struct BruteChoices {
    struct Row { uint64_t end, low, high; int count; };
    std::vector<Row> rows;
    Seq base;
    uint64_t begin = 0, end = 0, count = 0;
    unsigned vyKnown = 0;
    double velocities[8] = {}, vy = 0;
    int frames = 0;

    void Reset() { begin = end = count = 0; vyKnown = 0; rows.clear(); }

    bool Get(const Node& n, uint64_t index, const Sim& incoming, Sim& probe,
             Seq& out, Scratch& scratch, uint64_t& skip) {
        if (index < begin || index >= end) {
            const size_t L = (size_t)(std::upper_bound(n.cum.begin(), n.cum.end(), index) - n.cum.begin() - 1);
            uint64_t rank = index - n.cum[L];
            const uint64_t width = n.ways[0][L], baseIndex = rank / width;
            rank %= width;
            frames = (int)(std::upper_bound(n.ways[1].begin(), n.ways[1].end(), rank) - n.ways[1].begin() - 1);
            begin = n.cum[L] + baseIndex * width + n.ways[1][(size_t)frames];
            end = n.cum[L] + baseIndex * width + n.ways[1][(size_t)frames + 1];
            base.clear();
            n.kids[0]->GetByLen((long)L, baseIndex, base, scratch);
            const unsigned direction = base[0] & 5;
            if (!(vyKnown & (1u << direction))) {
                std::memcpy(&probe, &incoming, sizeof probe);
                probe.Tick((uint8_t)direction); // BruteStart's one-tick lookahead
                velocities[direction] = probe.world.bodies[probe.playerBody].linearVelocity.y;
                vyKnown |= 1u << direction;
            }
            vy = velocities[direction];
            rows.clear();
            count = BruteRows(n, frames, vy, [&](int k, uint64_t low, uint64_t high, uint64_t end) {
                rows.push_back({end, low, high, k});
            });
        }
        skip = end;
        uint64_t rank = index - begin;
        if (rank >= count) return false;
        const auto row = std::upper_bound(rows.begin(), rows.end(), rank,
            [](uint64_t r, const Row& x) { return r < x.end; });
        if (row != rows.begin()) rank -= (row - 1)->end;
        out = base;
        // Like BruteEnv, replace Up within the marked window and retain L/R.
        // Inputs after the window, including any Up, remain exactly as written.
        for (int i = 0; i < frames; ++i) out[(size_t)i] &= 5;
        JumpBrute(frames, vy).Place(row->count, row->high - rank, [&](int i) { out[(size_t)i] |= IN_U; });
        return true;
    }
};

std::unique_ptr<Node> ParseSeq(Parser& p, const char* stops);

void ParseJumpPercent(Parser& p, Node& n) {
    if (!p.Int(n.jumpPercentHi)) p.Fail("reverse-index sum percentage expected");
    p.Skip();
    if (p.err.empty() && p.s.compare(p.i, 2, "..") == 0) {
        p.i += 2;
        n.jumpPercentLo = n.jumpPercentHi;
        if (!p.Int(n.jumpPercentHi)) p.Fail("percentage expected after ..");
    }
    if (p.err.empty() && (n.jumpPercentHi > 100 || n.jumpPercentLo > n.jumpPercentHi || !p.Eat('%')))
        p.Fail("reverse-index sum range must be i..j% with integers 0 <= i <= j <= 100 (x% means 0..x%)");
}

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
        n->position = p.i - 1;
        n->kids.push_back(ParseSeq(p, ")|>:+"));
        if (!p.err.empty()) return n;
        n->add = p.Eat('+');
        if (!n->add && !p.Eat(':')) {
            p.Fail("expected the form <BASE:a3>, <BASE:a1..3,w1>, <BASE+a3> or <BASE+j>");
            return n;
        }
        if (p.Eat('b')) {
            n->kind = Node::BRUTE;
            if (!n->add) p.Fail("use +b to retain the base string's Left/Right inputs");
            p.Range(n->bruteLo, n->bruteHi, -1);
            if (n->bruteLo < 1 || n->bruteHi > 100000) p.Fail("b needs a frame count or range between 1 and 100000");
            n->lo = 0, n->hi = LONG_MAX;
            if (p.err.empty() && p.Eat(',')) {
                if (p.Eat('j')) {
                    p.Range(n->lo, n->hi, -1);
                    if (n->lo < 0) n->lo = 0, n->hi = LONG_MAX;
                    if (p.err.empty() && p.Eat(',')) ParseJumpPercent(p, *n);
                } else ParseJumpPercent(p, *n);
            }
            if (p.err.empty() && !p.Eat('>')) p.Fail("expected <BASE+b10..20, j5..6, 30..40%>");
            return n;
        }
        if (p.Eat('j')) {
            n->kind = Node::JUMP;
            n->code = IN_U;
            p.Range(n->lo, n->hi, -1);
            if (n->lo < 0) n->lo = 0, n->hi = LONG_MAX;
            if (p.err.empty() && p.Eat(',')) ParseJumpPercent(p, *n);
            if (p.err.empty() && !p.Eat('>')) p.Fail("'>' expected after jump count or percentage cap");
            return n;
        }
        do {
            if (p.Eat('r')) {
                if (n->taps.size() != 1) {
                    p.Fail("r requires exactly one tap kind; use nested blocks for separate reductions");
                    return n;
                }
                // The same static count/sum representatives as j, for the
                // requested input code. No game state or vy test is involved.
                n->kind = Node::REDUCED;
                n->code = n->taps[0].code;
                n->lo = n->taps[0].lo, n->hi = n->taps[0].hi;
                n->taps.clear();
                if (p.Eat(',')) ParseJumpPercent(p, *n);
                if (p.err.empty() && !p.Eat('>')) p.Fail("expected <BASE+a1..5, r> or <BASE+a1..5, r, 30..40%>");
                return n;
            }
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
    if (p.Eat('{')) {
        const size_t close = p.s.find('}', p.i);
        if (close == std::string::npos) {
            p.Fail("'}' expected after the file name");
            return n;
        }
        std::string name = p.s.substr(p.i, close - p.i);
        p.i = close + 1;
        long top = LONG_MAX;
        const size_t colon = name.find_last_of(':');
        if (colon != std::string::npos && colon + 1 < name.size() && name.find_first_not_of("0123456789 ", colon + 1) == std::string::npos)
            top = std::atol(name.c_str() + colon + 1), name.resize(colon);
        name.erase(0, name.find_first_not_of(" \t")), name.erase(name.find_last_not_of(" \t") + 1);
        std::ifstream f(name);
        if (!f) {
            p.Fail("cannot read " + name);
            return n;
        }
        // Every line that is a whole route (letters with counts: a --save file's input lines, --list output).
        auto isRoute = [](const std::string& t) {
            if (t.empty() || !std::strchr("ndweaSqW", t[0])) return false;
            for (size_t k = 0; k < t.size(); ++k)
                if (std::isdigit((unsigned char)t[k]) ? false : !std::strchr("ndweaSqW", t[k]) || k + 1 == t.size() || !std::isdigit((unsigned char)t[k + 1]))
                    return false;
            return true;
        };
        n->kind = Node::LIST;
        for (std::string line; (long)n->list.size() < top && std::getline(f, line);) {
            line.erase(0, line.find_first_not_of(" \t")), line.erase(line.find_last_not_of(" \t\r") + 1);
            if (isRoute(line)) n->list.push_back(DecodeInputs(line));
        }
        if (n->list.empty()) p.Fail("no routes in " + name);
        while (p.err.empty() && p.Eat('[')) {
            const bool pad = p.Eat('+');
            if (!pad && !p.Eat('-')) {
                p.Fail("file suffix must be [+N] or [-N]");
                break;
            }
            long frames;
            if (!p.Int(frames) || !p.Eat(']')) {
                p.Fail("frame count followed by ']' expected in file suffix");
                break;
            }
            for (Seq& q : n->list) {
                if (pad) {
                    if ((size_t)frames > MAX_LEN || q.size() > MAX_LEN - (size_t)frames) {
                        p.Fail("padded file string longer than 1,000,000 frames");
                        break;
                    }
                    q.resize(q.size() + (size_t)frames, 0);
                } else q.resize(q.size() - std::min(q.size(), (size_t)frames));
            }
        }
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

struct PreparedBrute;
struct Segment {
    std::unique_ptr<Node> node;
    std::shared_ptr<const PreparedBrute> prepared;
    uint64_t preparedSize = 0;
    uint64_t size() const { return prepared ? preparedSize : node->total; }
    bool generated() const { return prepared || !node->stored; }
    bool needsState() const { return node->kind == Node::BRUTE && !prepared; }
    void Append(uint64_t i, Seq& out, Scratch& sc, size_t* prefixEnd = nullptr) const;
    void Get(uint64_t i, Seq& out, Scratch& sc, size_t* prefixEnd = nullptr) const {
        out.clear(); Append(i, out, sc, prefixEnd);
    }
};

// A dense, immutable generator: each entry is one prefix/base/window group.
// It retains only an endpoint and a probed velocity, never a game savestate.
struct PreparedBrute {
    struct Group { uint64_t base; int frames; unsigned direction; };
    struct Entry { uint64_t end = 0; double vy = 0; };
    std::vector<Segment> prefix;
    std::vector<uint64_t> strides;
    std::vector<Group> groups;
    std::vector<Entry> entries;

    void Append(const Node& n, uint64_t index, Seq& out, Scratch& scratch, size_t* prefixEnd) const {
        const auto entry = std::upper_bound(entries.begin(), entries.end(), index,
            [](uint64_t k, const Entry& e) { return k < e.end; });
        const size_t slot = (size_t)(entry - entries.begin());
        const uint64_t prefixIndex = slot / groups.size();
        const Group& group = groups[slot % groups.size()];
        uint64_t rank = index - (slot ? entries[slot - 1].end : 0);
        for (size_t i = 0; i < prefix.size(); ++i)
            prefix[i].Append(prefixIndex / strides[i] % prefix[i].size(), out, scratch);
        const size_t start = out.size();
        if (prefixEnd) *prefixEnd = start;
        n.kids[0]->Append(group.base, out, scratch);
        for (int i = 0; i < group.frames; ++i) out[start + (size_t)i] &= 5;
        bool placed = false;
        BruteRows(n, group.frames, entry->vy, [&](int k, uint64_t low, uint64_t high, uint64_t) {
            if (placed) return;
            const uint64_t count = high - low + 1;
            if (rank >= count) { rank -= count; return; }
            JumpBrute(group.frames, entry->vy).Place(k, high - rank,
                [&](int i) { out[start + (size_t)i] |= IN_U; });
            placed = true;
        });
    }
};

void Segment::Append(uint64_t i, Seq& out, Scratch& sc, size_t* prefixEnd) const {
    if (prefixEnd) *prefixEnd = 0;
    if (prepared) prepared->Append(*node, i, out, sc, prefixEnd);
    else node->Append(i, out, sc);
}

std::vector<Segment> ParsePattern(const std::string& s, double budget, double& usedBytes, std::string& err,
                                std::vector<JumpSummary>& jumps) {
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
    std::sort(p.jumps.begin(), p.jumps.end(), [](const JumpSummary& a, const JumpSummary& b) { return a.position < b.position; });
    jumps = std::move(p.jumps);
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

enum class PrepareResult { Skipped, Ready, Empty, Stopped };

// Turn the first remaining state-dependent block and its prefix into a static
// generator. Sampling can then shuffle actual candidates instead of replaying
// a prefix merely to discover that its slot was excluded by vy or percentage.
PrepareResult PrepareBrute(std::vector<Segment>& segs, const Sim& origin, int threads,
                           double budget, double& storedBytes, double timeLimit,
                           double& seconds, bool& timedOut, std::vector<JumpSummary>* summaries = nullptr) {
    size_t at = 0;
    while (at < segs.size() && !segs[at].needsState()) ++at;
    if (at == segs.size()) return PrepareResult::Skipped;
    const Node& n = *segs[at].node;
    uint64_t prefixes = 1;
    std::vector<uint64_t> strides(at);
    for (size_t i = at; i-- > 0;) {
        strides[i] = prefixes;
        if (segs[i].size() >= SAT / prefixes) return PrepareResult::Skipped;
        prefixes *= segs[i].size();
    }
    const long first = std::max(n.bruteLo, n.lo >= 100000 ? 100001L : n.lo + 1);
    uint64_t groupCount = 0;
    double rowWork = 0;
    const Node& child = *n.kids[0];
    for (size_t L = 0; L < child.cnt.size(); ++L) if (child.cnt[L]) {
        const long last = std::min<long>(n.bruteHi, (long)L);
        if (last < first) continue;
        groupCount = SatAdd(groupCount, SatMul(child.cnt[L], (uint64_t)(last - first + 1)));
        rowWork += (double)child.cnt[L] * (last - first + 1) * (std::min(n.hi, last - 1) - n.lo + 1);
    }
    const double tableBytes = (double)prefixes * groupCount * sizeof(PreparedBrute::Entry) +
                             (double)groupCount * sizeof(PreparedBrute::Group) +
                             (double)at * (sizeof(Segment) + sizeof(uint64_t)) + sizeof(PreparedBrute);
    const int workers = (int)std::min<uint64_t>((uint64_t)threads, prefixes);
    // Bound both retained scalar tables and temporary probing states. A large
    // dependent tree keeps the existing on-demand path instead of expanding it.
    if (!groupCount || tableBytes + 2.0 * workers * sizeof(Sim) > std::max(0.0, budget - storedBytes) * 0.15 ||
        rowWork * prefixes > Parser::MAX_WORK) return PrepareResult::Skipped;

    auto plan = std::make_shared<PreparedBrute>();
    plan->strides = std::move(strides);
    plan->groups.reserve((size_t)groupCount);
    unsigned directions = 0;
    {
        Seq base;
        Scratch scratch;
        for (uint64_t b = 0; b < child.total; ++b) {
            child.Get(b, base, scratch);
            for (long frames = first; frames <= std::min<long>(n.bruteHi, (long)base.size()); ++frames) {
                const unsigned direction = base[0] & 5;
                plan->groups.push_back({b, (int)frames, direction});
                directions |= 1u << direction;
            }
        }
    }
    plan->entries.resize((size_t)prefixes * plan->groups.size());
    Say("b: probing %llu prefixes once before candidate generation\n", (unsigned long long)prefixes);
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    std::atomic<uint64_t> next{0}, completed{0};
    std::atomic<int> running{workers};
    std::atomic<bool> expired{false}, failed{false};
    const auto stop = [&] {
        if (g_stop.load(std::memory_order_relaxed) || failed.load(std::memory_order_relaxed)) return true;
        if (timeLimit > 0 && seconds + elapsed() >= timeLimit) expired = true;
        return expired.load(std::memory_order_relaxed);
    };
    std::vector<std::exception_ptr> errors((size_t)workers);
    struct Witness { bool found = false; size_t slot = 0; int presses = 0; uint64_t sum = 0; };
    struct Extremes { Witness minimum, maximum; };
    std::vector<Extremes> extremes((size_t)workers);
    const auto offer = [](Witness& saved, size_t slot, int presses, uint64_t sum, bool maximum) {
        if (!saved.found || (maximum ? sum > saved.sum : sum < saved.sum) ||
            (sum == saved.sum && std::make_pair(slot, presses) < std::make_pair(saved.slot, saved.presses)))
            saved = {true, slot, presses, sum};
    };
    std::vector<std::thread> pool;
    struct SignalGuard {
        void (*previous)(int);
        ~SignalGuard() { std::signal(SIGINT, previous); }
    } signal{std::signal(SIGINT, OnInterrupt)};
    try { for (int worker = 0; worker < workers; ++worker) pool.emplace_back([&, worker] {
        try {
            auto state = std::make_unique<Sim>(), probe = std::make_unique<Sim>();
            Seq inputs;
            Scratch scratch;
            for (uint64_t p; !stop() && (p = next.fetch_add(1)) < prefixes;) {
                CopySim(*state, origin);
                inputs.clear();
                for (size_t i = 0; i < at; ++i)
                    segs[i].Append(p / plan->strides[i] % segs[i].size(), inputs, scratch);
                for (size_t i = 0; i < inputs.size(); ++i) {
                    if ((i & 63) == 0 && stop()) break;
                    state->Tick(inputs[i]);
                }
                if (stop()) break;
                double vy[8] = {};
                for (unsigned direction : {0u, 1u, 4u, 5u}) if (directions & (1u << direction)) {
                    CopySim(*probe, *state);
                    probe->Tick((uint8_t)direction);
                    vy[direction] = probe->world.bodies[probe->playerBody].linearVelocity.y;
                }
                for (size_t i = 0; i < plan->groups.size(); ++i) {
                    if ((i & 63) == 0 && stop()) break;
                    const auto& group = plan->groups[i];
                    const size_t slot = (size_t)p * plan->groups.size() + i;
                    auto& entry = plan->entries[slot];
                    entry.vy = vy[group.direction];
                    entry.end = BruteRows(n, group.frames, entry.vy, [&](int k, uint64_t low, uint64_t high, uint64_t) {
                        offer(extremes[(size_t)worker].minimum, slot, k, low, false);
                        offer(extremes[(size_t)worker].maximum, slot, k, high, true);
                    });
                }
                if (!stop()) ++completed;
            }
        } catch (...) { errors[(size_t)worker] = std::current_exception(); failed = true; }
        --running;
    }); } catch (...) {
        failed = true;
        for (auto& thread : pool) thread.join();
        throw;
    }
    double nextUpdate = 5;
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (elapsed() >= nextUpdate) {
            Say("b: probed %llu / %llu prefixes\n", (unsigned long long)completed.load(), (unsigned long long)prefixes);
            nextUpdate += 5;
        }
    }
    for (auto& thread : pool) thread.join();
    const double duration = elapsed();
    seconds += duration;
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    timedOut = expired.load();
    if (g_stop || timedOut) return PrepareResult::Stopped;
    uint64_t count = 0;
    for (auto& entry : plan->entries) entry.end = count = SatAdd(count, entry.end);
    if (count >= SAT) return PrepareResult::Skipped;
    if (summaries) {
        Extremes all;
        for (const auto& e : extremes) {
            if (e.minimum.found) offer(all.minimum, e.minimum.slot, e.minimum.presses, e.minimum.sum, false);
            if (e.maximum.found) offer(all.maximum, e.maximum.slot, e.maximum.presses, e.maximum.sum, true);
        }
        const auto route = [&](const Witness& w) {
            if (!w.found) return std::string("none");
            const auto& group = plan->groups[w.slot % plan->groups.size()];
            Seq input;
            Scratch scratch;
            child.Get(group.base, input, scratch);
            for (int i = 0; i < group.frames; ++i) input[(size_t)i] &= 5;
            JumpBrute(group.frames, plan->entries[w.slot].vy).Place(w.presses, w.sum,
                [&](int i) { input[(size_t)i] |= IN_U; });
            return EncodeInputsRLE(input);
        };
        for (auto& summary : *summaries) if (summary.position == n.position)
            summary.minimum = route(all.minimum), summary.maximum = route(all.maximum), summary.stateDependent = false;
    }
    Say("b: %llu allowed choices, %.2f s preparation; generation now uses only cached velocities and counts\n",
        (unsigned long long)count, duration);
    if (!count) return PrepareResult::Empty;

    plan->prefix.reserve(at);
    for (size_t i = 0; i < at; ++i) plan->prefix.push_back(std::move(segs[i]));
    Segment combined = std::move(segs[at]);
    combined.prepared = std::move(plan);
    combined.preparedSize = count;
    segs.erase(segs.begin(), segs.begin() + at + 1);
    segs.insert(segs.begin(), std::move(combined));
    storedBytes += tableBytes;
    return PrepareResult::Ready;
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

struct SaveError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Each search owns a new directory beside its output. Workers use separate files;
// only allocating a file name is shared. Failed saves leave their runs recoverable.
struct ResultScratch {
    std::string folder;
    std::atomic<uint64_t> next{0};
    bool clean = false;

    explicit ResultScratch(const std::string& output) {
        const std::string base = output + ".parts";
        for (uint64_t n = 0;; ++n) {
            folder = base + (n ? "-" + std::to_string(n + 1) : "");
            std::error_code ec;
            if (std::filesystem::create_directory(folder, ec)) break;
            if (ec && ec != std::errc::file_exists) throw SaveError("cannot create temporary results in " + folder + ": " + ec.message());
        }
    }
    ~ResultScratch() {
        if (clean) {
            std::error_code ec;
            std::filesystem::remove(folder, ec);
        } else std::fprintf(stderr, "--save: temporary results retained in %s\n", folder.c_str());
    }
    std::string NewPath() { return folder + "/" + std::to_string(next.fetch_add(1)) + ".bin"; }
};

struct ResultRun {
    ResultScratch& scratch;
    std::string path;
    uint64_t rows = 0;
    explicit ResultRun(ResultScratch& s) : scratch(s), path(s.NewPath()) {}
    void Discard() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        path.clear();
    }
    ~ResultRun() { if (scratch.clean && !path.empty()) Discard(); }
};
using Run = std::unique_ptr<ResultRun>;

struct ResultFile {
    FILE* f;
    std::string path;
    struct Header {
        uint64_t length;
        int frame, alive;
        double x, y, vx, vy, flag, count;
    };
    ResultFile(const std::string& p, const char* mode) : f(std::fopen(p.c_str(), mode)), path(p) {
        if (!f) throw SaveError("cannot open temporary results " + path);
    }
    ~ResultFile() { if (f) std::fclose(f); }
    void Close() {
        FILE* old = f;
        f = nullptr;
        if (std::fclose(old)) throw SaveError("cannot close temporary results " + path);
    }
    void Write(const Match& m) {
        const Header h{(uint64_t)m.route.size(), m.frame, m.alive, m.x, m.y, m.vx, m.vy, m.flag, m.count};
        if (std::fwrite(&h, 1, sizeof h, f) != sizeof h || std::fwrite(m.route.data(), 1, m.route.size(), f) != m.route.size())
            throw SaveError("cannot write temporary results " + path);
    }
    bool Read(Match& m) {
        Header h{};
        const size_t got = std::fread(&h, 1, sizeof h, f);
        if (!got && !std::ferror(f)) return false;
        if (got != sizeof h || h.length > m.route.max_size()) throw SaveError("cannot read temporary results " + path);
        m.route.resize((size_t)h.length);
        if (std::fread(m.route.data(), 1, m.route.size(), f) != m.route.size()) throw SaveError("incomplete temporary results " + path);
        m.frame = h.frame, m.alive = h.alive != 0;
        m.x = h.x, m.y = h.y, m.vx = h.vx, m.vy = h.vy, m.flag = h.flag, m.count = h.count;
        return true;
    }
};

// Binary carries merge equal-sized batches, so the number of run descriptors is
// logarithmic and a merge needs just two readers and one writer, however many rows.
struct ResultRuns {
    ResultScratch& scratch;
    bool byRoute;
    std::vector<Run> levels;
    ResultRuns(ResultScratch& s, bool routes) : scratch(s), byRoute(routes) {}

    Run Merge(Run a, Run b) {
        auto out = std::make_unique<ResultRun>(scratch);
        ResultFile left(a->path, "rb"), right(b->path, "rb"), dest(out->path, "wb");
        Match x, y, pending;
        bool hasX = left.Read(x), hasY = right.Read(y), havePending = false;
        auto write = [&]() { dest.Write(pending); ++out->rows; };
        while (hasX || hasY) {
            const bool takeX = !hasY || (hasX && (byRoute ? x.route <= y.route : !Before(y, x)));
            Match& m = takeX ? x : y;
            if (havePending && pending.route == m.route) pending.count += m.count;
            else {
                if (havePending) write();
                pending = std::move(m), havePending = true;
            }
            if (takeX) hasX = left.Read(x);
            else hasY = right.Read(y);
        }
        if (havePending) write();
        dest.Close(), left.Close(), right.Close();
        a->Discard(), b->Discard();
        return out;
    }
    void AddRun(Run run, size_t level = 0) {
        for (;;) {
            if (levels.size() <= level) levels.resize(level + 1);
            if (!levels[level]) { levels[level] = std::move(run); return; }
            run = Merge(std::move(levels[level]), std::move(run));
            ++level;
        }
    }
    template<class Fill> void AddBlock(Fill fill) {
        auto run = std::make_unique<ResultRun>(scratch);
        ResultFile dest(run->path, "wb");
        fill([&](const Match& m) { dest.Write(m); ++run->rows; });
        dest.Close();
        AddRun(std::move(run));
    }
    void Take(ResultRuns& other) {
        for (size_t i = 0; i < other.levels.size(); ++i)
            if (other.levels[i]) AddRun(std::move(other.levels[i]), i);
    }
    Run Finish() {
        Run result;
        for (Run& run : levels)
            if (run) result = result ? Merge(std::move(result), std::move(run)) : std::move(run);
        return result;
    }
};

// Aggregate by the exact route first, then sort the unique results by --sortby.
// Neither phase retains the complete result set in RAM.
Run SortSavedResults(ResultRuns& routes, size_t batchSize, size_t byteCap = SIZE_MAX) {
    Run input = routes.Finish();
    if (!input) return nullptr;
    ResultRuns sorted(routes.scratch, false);
    ResultFile source(input->path, "rb");
    std::vector<Match> batch;
    size_t bytes = 0;
    auto flush = [&]() {
        std::sort(batch.begin(), batch.end(), Before);
        sorted.AddBlock([&](auto write) { for (const Match& m : batch) write(m); });
        batch.clear();
        bytes = 0;
    };
    Match m;
    while (source.Read(m)) {
        const size_t cost = 2 * sizeof(Match) + m.route.capacity();
        if (!batch.empty() && cost > byteCap - std::min(byteCap, bytes)) flush();
        batch.push_back(std::move(m));
        bytes += cost;
        if (batch.size() >= batchSize || bytes >= byteCap) flush();
    }
    if (!batch.empty()) flush();
    source.Close();
    input->Discard();
    return sorted.Finish();
}

struct Rows {
    size_t cap = SIZE_MAX;
    size_t byteCap = SIZE_MAX, bytes = 0;
    ResultRuns* spill = nullptr;
    bool evicted = false;
    double matched = 0;
    std::map<std::string, Match> byRoute;
    struct Ord {
        bool operator()(const Match* a, const Match* b) const { return Before(*a, *b); }
    };
    std::set<const Match*, Ord> order;

    void Flush() {
        if (byRoute.empty()) return;
        spill->AddBlock([&](auto write) { for (const auto& kv : byRoute) write(kv.second); });
        order.clear();
        byRoute.clear();
        bytes = 0;
    }
    void Add(Match&& m, bool count = true) {
        if (count) matched += m.count;
        auto it = byRoute.find(m.route);
        if (it != byRoute.end()) {
            it->second.count += m.count;
            return;
        }
        const size_t cost = (size_t)ROW_BYTES + 2 * m.route.capacity();
        if (spill) {
            if (!byRoute.empty() && (byRoute.size() >= cap || cost > byteCap - std::min(byteCap, bytes))) Flush();
            if (cost > byteCap) {
                spill->AddBlock([&](auto write) { write(m); });
                return;
            }
            bytes += cost;
        }
        if (byRoute.size() >= cap && !order.empty() && !Before(m, **order.rbegin())) {
            evicted = true;
            return;
        }
        const std::string key = m.route;
        auto res = byRoute.emplace(key, std::move(m));
        if (spill) return;
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

void SaveText(FILE* f, const std::string& text, const std::string& path) {
    if (std::fwrite(text.data(), 1, text.size(), f) != text.size()) throw SaveError("writing " + path + " failed");
}

void CloseSaved(FILE*& f, const std::string& path) {
    const bool good = !std::ferror(f);
    FILE* old = f;
    f = nullptr;
    const int closed = std::fclose(old);
    if (!good || closed) throw SaveError("writing " + path + " failed");
}


struct SharedPrefixes;
struct DeterministicPrefixes;
constexpr double PREFIX_MIN_SUFFIXES = 10;

// Complete input strings, compared byte-for-byte after decoding RLE. No prefix
// or game-state equivalence is implied, and no hash collision can exclude a route.
struct ExcludedStrings {
    std::unordered_set<std::string> strings;
    double bytes = 0;
    size_t longest = 0;
    bool Load(const std::string& path, double budget, std::string& error) {
        std::ifstream file(path);
        if (!file) { error = "cannot read " + path; return false; }
        for (std::string line; std::getline(file, line);) {
            const std::string text = Trim(line);
            size_t at = 0, length = 0;
            bool route = !text.empty();
            while (route && at < text.size()) {
                if (!std::strchr("ndweaSqW", text[at++]) || at == text.size() ||
                    !std::isdigit((unsigned char)text[at])) { route = false; break; }
                size_t count = 0;
                while (at < text.size() && std::isdigit((unsigned char)text[at]))
                    count = std::min(MAX_LEN + 1, count * 10 + (size_t)(text[at++] - '0'));
                length += count;
                if (length > MAX_LEN) { error = "excluded string longer than 1,000,000 frames"; return false; }
            }
            if (!route) continue; // saved-result headings and numeric rows
            if (bytes + StringBytes(length) + 32 > budget) { error = "excluded strings exceed --maxmem"; return false; }
            const Seq decoded = DecodeInputs(text);
            if (strings.emplace(decoded.begin(), decoded.end()).second) {
                bytes += StringBytes(length) + 32;
                longest = std::max(longest, decoded.size());
            }
        }
        if (file.bad()) { error = "reading " + path + " failed"; return false; }
        if (strings.empty()) { error = "no routes in " + path; return false; }
        return true;
    }
};

struct Force {
    std::vector<Segment> segs;
    std::vector<Cond> conds;
    struct Group {
        size_t a, b;     // conds[a, b): all on one frame
        int hi;          // last frame they can hold
        bool deadLater;  // this group and every later one can still be met with the ball dead
    };
    std::vector<Group> groups;  // met in order (--report "A, B; C"), each on a later frame
    std::vector<double> rest;
    int pad = 0;
    size_t split = 0;
    bool idleUp = false, rawStates = false, sampling = false;
    bool stateDependent = false;
    size_t lastBrute = 0;
    const Sim* origin = nullptr;
    double tol = 1e-10;
    size_t rowCap = SIZE_MAX;
    size_t rowByteCap = SIZE_MAX;
    ResultScratch* saved = nullptr;
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
    DeterministicPrefixes* deterministicPrefixes = nullptr;  // exact task prefixes, shared by all workers
    const ExcludedStrings* excluded = nullptr;

    // Random samples can only merge once few continuations are left.
    bool WorthClaim(size_t nextSeg) const { return !sampling || rest[nextSeg] <= (double)played->load(std::memory_order_relaxed) + 1; }
    bool MidClaims(size_t seg) const { return seen && mergeEvery > 0 && WorthClaim(seg + 1) && (!stateDependent || seg >= lastBrute); }
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

// A deterministic task fixes every option before split-1. Several tasks can have
// the same fixed prefix but different suffix ranges; do its physics only once.
// The key includes ALL earlier choices, not just a file row or its input text.
struct DeterministicPrefixes {
    using Id = std::pair<size_t, uint64_t>;  // segment, mixed-radix prefix index
    struct Entry {
        State state;
        std::vector<Key> midKeys;  // state keys only; the remaining-input hash belongs to each task
        Status result = GOING;
        size_t stopAt = 0, cost = 0;
        std::list<Id>::iterator age;
        std::mutex mutex;
        std::condition_variable ready;
        bool complete = false;
        std::exception_ptr failure;
    };
    const size_t capacity, entryLimit;
    std::mutex mutex;
    std::map<Id, std::shared_ptr<Entry>> entries;
    std::list<Id> ages;
    size_t used = 0, peakEntries = 0;
    uint64_t built = 0, reused = 0, evicted = 0;

    DeterministicPrefixes(size_t budget, size_t threads)
        : capacity(budget > sizeof(DeterministicPrefixes) ? budget - sizeof(DeterministicPrefixes) : 0), entryLimit(threads) {}

    static size_t Cost(size_t played, size_t keys) {
        // Includes the shared ownership block, tree/list nodes and allocation overhead.
        const size_t fixed = sizeof(Entry) + 256;
        if (played > SIZE_MAX - fixed || keys > (SIZE_MAX - fixed - played) / sizeof(Key)) return SIZE_MAX;
        return fixed + played + keys * sizeof(Key);
    }

    template<class Fill>
    std::shared_ptr<const Entry> Get(const Id& id, size_t cost, Fill&& fill) {
        std::shared_ptr<Entry> entry;
        bool owner = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = entries.find(id);
            if (found != entries.end()) {
                entry = found->second;
                ages.splice(ages.end(), ages, entry->age);
                ++reused;
            } else {
                if (entryLimit == 0 || cost > capacity) return {};
                // Keep in-use entries pinned, including one still being calculated.
                // Both ready and in-progress entries count toward the thread cap.
                // Evict before allocating a replacement, so even transient entries
                // cannot exceed it. If all slots are pinned, replay normally.
                const auto full = [&] { return entries.size() >= entryLimit || cost > capacity - used; };
                for (auto age = ages.begin(); full() && age != ages.end();) {
                    auto old = entries.find(*age);
                    if (old->second.use_count() != 1) { ++age; continue; }
                    used -= old->second->cost;
                    entries.erase(old);
                    age = ages.erase(age);
                    ++evicted;
                }
                if (full()) return {};
                entry = std::make_shared<Entry>();
                entry->cost = cost;
                entries.emplace(id, entry);
                try { ages.push_back(id); }
                catch (...) { entries.erase(id); throw; }
                entry->age = std::prev(ages.end());
                used += cost;
                peakEntries = std::max(peakEntries, entries.size());
                ++built;
                owner = true;
            }
        }
        if (owner) {
            try { fill(*entry); }
            catch (...) { entry->failure = std::current_exception(); }
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->complete = true;
            }
            entry->ready.notify_all();
        } else {
            std::unique_lock<std::mutex> lock(entry->mutex);
            entry->ready.wait(lock, [&] { return entry->complete; });
        }
        if (entry->failure) std::rethrow_exception(entry->failure);
        return entry;
    }
};

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
    std::unique_ptr<ResultRuns> saved;
    Rows rows;
    std::unique_ptr<Sim> canon = std::make_unique<Sim>();
    std::unique_ptr<Sim> decidedState;  // replay an early match held pending a later b block
    std::vector<Seq> buf, look;
    std::vector<BruteChoices> brute;
    Scratch scratch;
    double merged = 0;
    size_t cpLeft, stopAt = 0;
    Status ended = GOING;
    std::vector<std::unique_ptr<State>> lead;
    std::vector<uint8_t> leadAt;
    uint64_t sampleLead = 0;
    uint64_t excludedCount = 0;
    std::string excludedInput;
    std::vector<size_t> preparedPrefixEnds;
    bool optionsReady = false;

    explicit Walker(const Force& s) : sc(s), resume(s.segs.size()), cpLeft(s.checkpoints) {
        for (size_t i = 0; i <= s.segs.size(); ++i) pool.push_back(std::make_unique<State>());
        buf.resize(s.segs.size() + 1);
        look.resize(2 * s.segs.size());
        brute.resize(s.segs.size());
        if (s.excluded) preparedPrefixEnds.resize(s.segs.size());
        rows.cap = s.rowCap;
        rows.byteCap = s.rowByteCap;
        if (s.saved) saved = std::make_unique<ResultRuns>(*s.saved, true), rows.spill = saved.get();
    }

    const Sim& Canon(const State& st) {
        if (sc.rawStates) return st.sim;
        Canonicalize(st.sim, *canon, sc.report != nullptr);
        return *canon;
    }

    bool Claim(State& st, size_t seg, uint64_t rest = 0, Key* stateKey = nullptr) {
        if (!sc.seen) return true;
        // A completed report is waiting for a later b block. Its earlier output
        // state may differ even if these continuation states converge.
        if (sc.stateDependent && st.met[sc.groups.size() - 1]) return true;
        const Sim& c = Canon(st);
        const Key s = StateKey(c, *sc.keyBase, st.met, sc.conds.size(), sc.tol);
        if (stateKey) *stateKey = s;
        const uint64_t at = Place(seg, rest);
        const Key k = PlaceKey(s, at);
        const bool fresh = sc.seen->Claim(k);
        if (fresh && sc.report) sc.report->Check(c, k, CoarseKey(c, at, st.met, sc.conds.size()), sc.tol);
        return fresh;
    }

    Status Step(State& st, uint8_t in, bool awaitBrute = false) {
        Sim& s = st.sim;
        const bool complete = st.met[sc.groups.size() - 1] != 0;
        if (complete && !awaitBrute) return MATCH;
        if (!complete && !s.playerAlive && s.frameCount >= s.deathFrame + RESPAWN_LAST_PAUSE_FRAMES) return FAIL;
        const int f = s.frameCount + 1;
        const bool alive = s.playerAlive;
        s.Tick(in);
        st.played.push_back(!sc.idleUp && alive && s.upIdle ? (uint8_t)(in & ~2) : in);
        const bool won = s.isTimeStop && DeathWarpFinishValid(s.deathFrame, s.winFrame);
        size_t g = 0;
        while (g < sc.groups.size() && st.met[g]) ++g;
        if (g == sc.groups.size()) return awaitBrute ? GOING : MATCH;
        bool holds = true;
        for (size_t j = sc.groups[g].a; j < sc.groups[g].b && holds; ++j) {
            const Cond& c = sc.conds[j];
            holds = f >= c.lo && f <= c.hi && Holds(c, s, won);
        }
        if (holds) {
            st.met[g] = f;
            if (++g == sc.groups.size()) return awaitBrute ? GOING : MATCH;
        }
        for (size_t h = g; h < sc.groups.size(); ++h)
            if (f >= sc.groups[h].hi) return FAIL;
        if (s.isTimeStop || (!s.playerAlive && !sc.groups[g].deadLater)) return FAIL;
        return GOING;
    }

    const Seq* Option(const State& st, size_t seg, uint64_t k, Seq& out, uint64_t& skip, size_t* prefixEnd = nullptr) {
        const Segment& part = sc.segs[seg];
        const Node& n = *part.node;
        if (part.prepared) { part.Get(k, out, scratch, prefixEnd); return &out; }
        if (n.kind == Node::BRUTE)
            return brute[seg].Get(n, k, st.sim, *canon, out, scratch, skip) ? &out : nullptr;
        if (n.stored) return &n.list[(size_t)k];
        n.Get(k, out, scratch);
        return &out;
    }

    const Seq* NextOption(const State& st, size_t seg, uint64_t& k, uint64_t hi, Seq& out) {
        while (k < hi && !g_stop.load(std::memory_order_relaxed)) {
            uint64_t skip = k + 1;
            if (const Seq* q = Option(st, seg, k, out, skip)) return q;
            k = skip;
        }
        return nullptr;
    }

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
    Status Play(State& st, const Seq& in, size_t seg, uint64_t later, size_t from = 0, Resume* keep = nullptr, size_t mark = 0,
                size_t prefixEnd = 0) {
        const bool check = sc.MidClaims(seg);
        const size_t every = (size_t)sc.mergeEvery;
        // Shared prefixes only where no merge check falls inside them.
        SharedPrefixes::Id prefixKey[3];
        size_t cacheCount = 0;
        if (sc.prefixes && sc.sampling && seg == sc.lead && from == 0) {
            uint64_t inputs = 0;
            size_t at = 0;
            for (size_t depth : SharedPrefixes::depths) {
                if (depth >= in.size() || (check && (every <= depth || (prefixEnd && prefixEnd <= depth))) ||
                    !sc.prefixes->Supports(depth)) break;
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
            const Status r = Step(st, in[j], sc.stateDependent && seg < sc.lastBrute);
            stopAt = j + 1;
            if (r != GOING) return r;
            for (size_t i = 0; i < cacheCount; ++i)
                if (j + 1 == SharedPrefixes::depths[i]) sc.prefixes->Insert(prefixKey[i], st, j + 1);
            if (keep && j + 1 == mark) Push(*keep, st, mark);
            // Prepared random choices contain their old prefix and jump in one
            // sequence. Keep the merge at their boundary, before replaying the
            // jump, and count its internal merge interval from that boundary.
            if (check && j + 1 < in.size() && j + 1 >= prefixEnd && (j + 1 - prefixEnd) % every == 0) {
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
        for (size_t j = 0; j < sc.conds.size(); ++j) m.frame = std::max(m.frame, st.met[j]);
        Sim* decided = &st.sim;
        size_t length = st.played.size();
        if (sc.stateDependent && (st.sim.frameCount != m.frame ||
            st.played.size() > (size_t)std::max(0, m.frame - sc.origin->frameCount))) {
            // The match was held pending while later state-dependent choices
            // were validated. Recover its original row without retaining a
            // second savestate for every branch or changing its deciding frame.
            if (!decidedState) decidedState = std::make_unique<Sim>();
            CopySim(*decidedState, *sc.origin);
            length = 0;
            while (length < st.played.size() && decidedState->frameCount < m.frame) decidedState->Tick(st.played[length++]);
            decided = decidedState.get();
        }
        Sim& s = *decided;
        if (sc.seen) {
            // MATCH returns before the ordinary boundary claim. Deduplicate
            // its actual saved endpoint, including early matches and padding.
            // No continuation or intermediate report history remains relevant.
            const Sim* keyState = &s;
            if (!sc.rawStates) {
                Canonicalize(s, *canon, sc.report != nullptr);
                keyState = canon.get();
            }
            const Key stateKey = StateKey(*keyState, *sc.keyBase, &m.frame, 1, sc.tol);
            // An extra boundary separates completed results from live branches.
            const uint64_t at = Place(sc.segs.size() + 1, 0);
            const Key key = PlaceKey(stateKey, at);
            if (!sc.seen->Claim(key)) {
                merged += sc.stateDependent ? 1 : count;
                return;
            }
            if (sc.report) sc.report->Check(*keyState, key,
                CoarseKey(*keyState, at, &m.frame, 1), sc.tol);
        }
        m.alive = s.playerAlive;
        if (m.alive) {
            m.x = Value(s, Q_X), m.y = Value(s, Q_Y), m.vx = Value(s, Q_VX), m.vy = Value(s, Q_VY);
        }
        m.flag = Value(s, Q_FLAG);
        // Future b blocks have not been expanded yet. Do not multiply this
        // early match by their static upper bounds and call it an exact count.
        m.count = sc.stateDependent ? 1 : count;
        if (length == st.played.size()) m.route = EncodeInputsRLE(st.played);
        else m.route = EncodeInputsRLE(Seq(st.played.begin(), st.played.begin() + length));
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
        brute[seg].Reset();
        uint64_t k = lo;
        size_t buffer = 0;
        const Seq* opt = NextOption(st, seg, k, hi, look[2 * seg]);
        const uint64_t first = k;
        while (opt && !g_stop.load(std::memory_order_relaxed)) {
            uint64_t nextK = k + 1;
            const Seq* next = NextOption(st, seg, nextK, hi, look[2 * seg + (buffer ^ 1)]);
            size_t from = 0, fail = 0, mark = 0;
            if (reuse) {
                if (k > first) {
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
                } else if (next) {
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
            k = nextK;
            buffer ^= 1;
        }
    }

    Status PlayFixedPrefix(State& st, const Seq& in, size_t seg, uint64_t prefixIndex, uint64_t later) {
        const bool check = sc.MidClaims(seg);
        const size_t every = (size_t)sc.mergeEvery;
        const size_t keys = check && !in.empty() ? (in.size() - 1) / every : 0;
        if (in.size() > SIZE_MAX - st.played.size()) return Play(st, in, seg, later);
        const size_t played = st.played.size() + in.size();
        const auto cached = sc.deterministicPrefixes->Get({seg, prefixIndex}, DeterministicPrefixes::Cost(played, keys),
            [&](DeterministicPrefixes::Entry& e) {
                e.state.played.reserve(played);
                e.midKeys.reserve(keys);
                CopyState(e.state, st);
                for (size_t j = 0; j < in.size(); ++j) {
                    e.result = Step(e.state, in[j], sc.stateDependent && seg < sc.lastBrute);
                    e.stopAt = j + 1;
                    if (e.result != GOING) break;
                    if (check && j + 1 < in.size() && (j + 1) % every == 0)
                        e.midKeys.push_back(StateKey(Canon(e.state), *sc.keyBase, e.state.met, sc.conds.size(), sc.tol));
                }
            });
        if (!cached) return Play(st, in, seg, later);
        // Never cache a MERGED decision. Reapply every skipped claim, in order,
        // with THIS task's remaining-input hash, even on the first cache use.
        // --merge-report uses the original path because it needs intermediate Sims.
        for (size_t k = 0; k < cached->midKeys.size(); ++k) {
            const size_t at = (k + 1) * every;
            if (!sc.seen->Claim(PlaceKey(cached->midKeys[k], Place(seg, RestOf(in, at, later))))) {
                stopAt = at;
                return MERGED;
            }
        }
        CopyState(st, cached->state);
        stopAt = cached->stopAt;
        return cached->result;
    }

    // weight: strings a match or merge stands for; tail: hash of what is still to play.
    bool PlayPrefix(State& st, const std::vector<uint64_t>& digit, size_t from, size_t upto, double weight, uint64_t tail = 0) {
        uint64_t prefixIndex = 0;
        if (sc.deterministicPrefixes)
            for (size_t j = 0; j < from; ++j) prefixIndex = prefixIndex * sc.segs[j].size() + digit[j];
        for (size_t i = from; i < upto; ++i) {
            Hasher todo(0x51ED270B27E84A3DULL);
            for (size_t j = i + 1; j < digit.size(); ++j) todo.Mix(digit[j] + 1);
            if (tail) todo.Mix(tail);
            const uint64_t later = i + 1 < digit.size() || tail ? todo.Done().a : 0;
            brute[i].Reset();
            uint64_t skip = digit[i] + 1;
            size_t prefixEnd = 0;
            const Seq* option;
            if (optionsReady) { option = &buf[i]; prefixEnd = preparedPrefixEnds[i]; }
            else option = Option(st, i, digit[i], buf[i], skip, sc.sampling ? &prefixEnd : nullptr);
            if (!option) { ended = FAIL; return false; }
            const Seq& in = *option;
            // Task splitting bounds this mixed-radix index by SAT. Walk already
            // saves states for the suffix tree inside each task; only task prefixes
            // need sharing. Count suffix choices, not --pad frames or file rows.
            if (sc.deterministicPrefixes) prefixIndex = prefixIndex * sc.segs[i].size() + digit[i];
            const Status r = sc.deterministicPrefixes && !sc.sampling && !sc.report &&
                             sc.rest[i + 1] >= PREFIX_MIN_SUFFIXES && in.size() >= MIN_SKIP
                ? PlayFixedPrefix(st, in, i, prefixIndex, later) : Play(st, in, i, later, 0, nullptr, 0, prefixEnd);
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
        optionsReady = false;
        if (sc.excluded) {
            excludedInput.clear();
            bool possible = true;
            for (size_t i = 0; i < sc.segs.size(); ++i) {
                sc.segs[i].Get(digit[i], buf[i], scratch, &preparedPrefixEnds[i]);
                if (buf[i].size() > sc.excluded->longest - excludedInput.size()) possible = false;
                if (possible) excludedInput.append(buf[i].begin(), buf[i].end());
            }
            if (possible && sc.excluded->strings.count(excludedInput)) { ++excludedCount; return; }
            // Reuse these generated parts during replay; no second generation.
            optionsReady = true;
        }
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
    SearchOffset startPercent;
    std::string startPercentText;
    std::string inputs, report = "win", excludeFile;
    Force sc;
    for (int i = 2; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--level")) level = std::atoi(next());
        else if (!std::strcmp(argv[i], "--inputs")) inputs = next();
        else if (!std::strcmp(argv[i], "--report")) report = next();
        else if (!std::strcmp(argv[i], "--exclude")) {
            excludeFile = next();
            if (excludeFile.empty()) { std::fprintf(stderr, "--exclude needs a file name\n"); return 2; }
        }
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
        else if (!std::strcmp(argv[i], "--start-percent")) {
            startPercentText = next();
            if (!startPercent.Parse(startPercentText)) {
                std::fprintf(stderr, "--start-percent needs 0..100 (at most nine decimal places; optional %% suffix)\n"); return 2;
            }
        }
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
    if (!startPercentText.empty() && !stochastic) {
        std::fprintf(stderr, "--start-percent requires --stochastic\n"); return 2;
    }
    if (startPercent.units && !seedGiven) {
        std::fprintf(stderr, "--start-percent above zero requires --seed to preserve the random ordering\n"); return 2;
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
            "                        [--merge-report] [--maxmem GB] [--stochastic [--time SEC] [--samples N] [--seed S] [--start-percent P]\n"
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
            "  <BASE+a1..5, r>  reduce one tap kind to one placement per press count and reverse-index sum, like j.\n"
            "              Uses no game state. Also accepts : to overwrite, other input letters, and an optional\n"
            "              percentage after r: <BASE+a1..5, r, 30..40%%>. Indices run from BASE length down to 1;\n"
            "              the maximum spans the whole count range. Counts refer to selected frames, even if\n"
            "              BASE already holds that key. Equal count/sum does not guarantee equal physics outcomes.\n"
            "  <BASE+j>    w presses as the jump brute-forcer places them, on BASE = the frames after take-off: one string\n"
            "              per number of presses and height reached (sum of the presses' frames counted from BASE's end),\n"
            "              not every subset; <BASE+j3>, <BASE+j2..5>: only that many presses; <BASE:j>: plain w\n"
            "  <BASE:j2..5, 25..50%%>  keep reverse-index sums in 25..50%% of the maximum across requested press counts,\n"
            "              for each BASE length (last frame = 1); inclusive, lower rounded up, upper down.\n"
            "              Integer endpoints 0..100; x%% means 0..x%%; also works with +j\n"
            "              Startup prints jump 1 - min: STRING, max: STRING for each jump block in pattern order.\n"
            "  <BASE+b10..20, j5..6, 30..40%%>  use the built-in, state-aware jump brute-forcer on the first 10, 11, ... 20\n"
            "              frames of BASE. Keep BASE's full length and its tail; replace Up inside the window, retain L/R.\n"
            "              Start after the take-off input. j counts generated Up frames; the percentage uses the maximum\n"
            "              eligible sum across that count range, separately for each incoming state and b duration.\n"
            "              Like the viewer, bN samples vy after the first tick; weighted indices are N-1 down to 1.\n"
            "              j and %% may be omitted (all). Windows longer than BASE are skipped. Use b blocks as separate\n"
            "              pattern parts, not nested. Prefixes are probed once when preparation fits the memory/work limits;\n"
            "              generation then uses cached vy values, reports exact totals, and prints each block's min/max.\n"
            "              Larger dependent trees fall back to on-demand probes and candidate-slot upper bounds.\n"
            "              --list also probes prefixes; prepared jump generation uses no further simulation.\n"
            "  {FILE}      every route in FILE, e.g. the results a --save run kept (best first): {l8.txt}<d20+j>d15\n"
            "              continues each of them; {FILE:100}: only the first 100\n"
            "  {FILE}[-N]  trim N frames from each imported route (at most its full length)\n"
            "  {FILE}[+N]  append N no-input (n) frames; suffixes can be chained; empty routes print as n0\n"
            "  e.g. \"d18(e1..6d0..5)=6<n4:a1>(d4..7e7..10)=14\" = 6 x 4 x 4 = 96 strings\n"
            "  Strings come shortest first, then in written order. Every part of the pattern is expanded in memory (repeats\n"
            "  removed) if it fits in --maxmem GB (default 4); a part that does not is generated one string at a time\n"
            "  instead (no limit, repeats kept; --hardfilter merges them).\n"
            "report: conditions joined by commas must all hold on the same frame; groups separated by ; are met one after\n"
            "  another, each on a later frame. Each condition may have a frame window @F, @A..B, @..B, @A..\n"
            "  win  dead  alive  ground (a jump probe touches)  contact  contact=NAME (--names lists them)\n"
            "  Q<V  Q<=V  Q>V  Q>=V  Q=V   Q: px py vx vy vr (as the mod's HUD and /set: metres, m/s), pr (rotation in\n"
            "                              degrees as the HUD shows it: fmod(angle, 360), negative when turned backwards),\n"
            "                              flag (distance from the ball's box to the flag's in twips, <= 0: touching)\n"
            "  e.g. --report \"contact=kickBall@40..60; px>=12.5, ground@80..\" (default: win)\n"
            "frames are the game's (the flag frame is the one on the game's timer). A string is decided on the frame the last group holds;\n"
            "the rest of it is not played. --pad N: N extra n frames after each string. --list: print the strings only.\n"
            "Rows (and the best kept) are ordered by frame, then flag; --sortby Q (frame px py vx vy flag) orders them by\n"
            "that column, highest first (--sortby -Q: lowest first; dead rows last for px py vx vy), ties by frame, flag.\n"
            "--save [FILE]: at the end (also after Ctrl+C) write every matching result to FILE, with no row cap.\n"
            "--exclude FILE: skip exact complete input strings from a saved-results or route-list file.\n"
            "  With --stochastic, excludes before physics replay; also works with --list. Other variants of the same\n"
            "  prefix remain. Compares decoded inputs as generated, before idle-Up removal and --pad. Search requires\n"
            "  all b blocks to fit preparation. Exclusions do not record or resume unsuccessful past candidates.\n"
            "Results spill to temporary disk files within the memory budget, then are combined and sorted by --sortby.\n"
            "--top limits the console only. With --list, --save writes every generated string without simulation.\n"
            "The file begins with the command; by default smartforce-L<level>-<date>-<time>.txt\n"
            "(FILE ending in / or \\: in that folder;\n"
            "folders are created). An existing file is never overwritten: -2, -3, ... is added to the name.\n"
            "Up held on a frame where it does nothing (ball awake, no jump: no contact or no foot probe, no lift: vy >= 0) is\n"
            "written as the same keys without Up (the game plays both identically). --idle-up keeps the strings as written.\n"
            "--hardfilter: strings in the same game state at the same point of the pattern, with the same inputs still to\n"
            "play, are merged: only the first is played on. States are compared without data the game never reads again\n"
            "(freed contacts, stale manifold points, caches); doubles count as equal within --tolerance T (default 1e-10;\n"
            "0: bit-exact only). With a tolerance the merge is a heuristic: a 1e-10 difference can grow. --merge-report: also\n"
            "list what still differs between states whose bodies agree to 1e-7 but that were not merged; --raw-states\n"
            "compares the full state instead. States are compared between the parts of the pattern and every\n"
            "--merge-every N frames inside a part (default 32; 0: only between parts), and at every saved result.\n"
            "--stochastic: play the strings in a uniformly random order without repeats and without storing them (a keyed\n"
            "bijection of all string numbers), until all are played or Ctrl+C (or --time SEC, --samples N); every\n"
            "--update SEC (default 60, 0: off) it prints how many strings it has played and how fast, then the 10 best\n"
            "so far. --seed S repeats a run. Ctrl+C stops any search and prints what was found.\n"
            "--start-percent P: start at floor(total * P / 100) in that same shuffled order, without replaying earlier\n"
            "strings. Requires --stochastic and an explicit --seed above 0%%; supports exact numbered domains only.\n"
            "P is 0..100, with up to nine decimal places and an optional %% suffix. Keep the same pattern, input files\n"
            "and generator version. --samples limits NEW visits; progress includes the skipped portion. Works with --list.\n"
            "For an estimated stopping percentage, start slightly earlier to avoid leaving a gap.\n");
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
        if (std::fflush(saveFile.f) || std::ferror(saveFile.f)) throw SaveError("writing " + saveName + " failed");
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
    ExcludedStrings exclusions;
    if (!excludeFile.empty()) {
        if (!stochastic && !list) {
            std::fprintf(stderr, "--exclude requires --stochastic for search (or --list)\n"); return 2;
        }
        if (!exclusions.Load(excludeFile, budget, err)) {
            std::fprintf(stderr, "--exclude: %s\n", err.c_str()); return 2;
        }
        sc.excluded = &exclusions;
        Say("exclude: loaded %zu exact strings from %s\n", exclusions.strings.size(), excludeFile.c_str());
    }
    double storedBytes = 0;
    std::vector<JumpSummary> jumps;
    sc.segs = ParsePattern(inputs, budget - exclusions.bytes, storedBytes, err, jumps);
    storedBytes += exclusions.bytes;
    if (!err.empty()) {
        std::fprintf(stderr, "pattern: %s\n", err.c_str());
        return 2;
    }
    if (!list) {
        for (size_t start = 0; start <= report.size();) {
            size_t end = report.find_first_of(",;", start);
            if (end == std::string::npos) end = report.size();
            const std::string tok = Trim(report.substr(start, end - start));
            const bool closesGroup = end == report.size() || report[end] == ';';
            start = end + 1;
            if (!tok.empty()) {
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
            const size_t a = sc.groups.empty() ? 0 : sc.groups.back().b;
            if (closesGroup && sc.conds.size() > a) sc.groups.push_back({a, sc.conds.size(), INT_MAX, true});
        }
        if (sc.conds.empty() || sc.conds.size() > MAX_CONDS) {
            std::fprintf(stderr, "report: 1 to %d conditions\n", MAX_CONDS);
            return 2;
        }
        for (size_t g = sc.groups.size(); g-- > 0;) {
            Force::Group& G = sc.groups[g];
            int lo = 1;
            bool dead = g + 1 == sc.groups.size() || sc.groups[g + 1].deadLater;
            for (size_t j = G.a; j < G.b; ++j) {
                lo = std::max(lo, sc.conds[j].lo), G.hi = std::min(G.hi, sc.conds[j].hi);
                dead = dead && CanHoldDead(sc.conds[j]);
            }
            G.deadLater = dead;
            if (lo > G.hi) {
                std::fprintf(stderr, "report: the conditions of group %zu (joined by commas: on the same frame) have no frame in common\n", g + 1);
                return 2;
            }
        }
    }

    double preparationSeconds = 0;
    bool preparationTimedOut = false;
    const auto printJumps = [&] {
        for (size_t i = 0; i < jumps.size(); ++i) {
            if (jumps[i].stateDependent) Say("jump %zu - min/max depend on the incoming game state\n", i + 1);
            else Say("jump %zu - min: %s, max: %s\n", i + 1, jumps[i].minimum.c_str(), jumps[i].maximum.c_str());
        }
    };
    for (;;) {
        const PrepareResult prepared = PrepareBrute(sc.segs, base->sim, threads, budget, storedBytes,
            stochastic && !list ? timeLimit : 0, preparationSeconds, preparationTimedOut, &jumps);
        if (prepared == PrepareResult::Skipped) break;
        if (prepared == PrepareResult::Ready) continue;
        if (prepared == PrepareResult::Empty) { printJumps(); Say("0 valid strings after jump filtering\n"); }
        else Say("stopped during jump preparation: %s\n", preparationTimedOut ? "time limit" : "Ctrl+C");
        if (saveFile.f) {
            SaveText(saveFile.f, prepared == PrepareResult::Empty ? "# no valid jump candidates\n" : "# stopped during jump preparation\n", saveName);
            CloseSaved(saveFile.f, saveName);
            std::fprintf(stderr, "saved 0 %s to %s\n", list ? "strings" : "rows", saveName.c_str());
        }
        return list ? 0 : 1;
    }
    printJumps();
    for (size_t i = 0; i < sc.segs.size(); ++i)
        if (sc.segs[i].needsState()) sc.stateDependent = true, sc.lastBrute = i;
    if (sc.excluded && sc.stateDependent && !list) {
        std::fprintf(stderr, "--exclude needs all b blocks prepared before search; increase --maxmem or reduce the pattern\n");
        return 2;
    }
    sc.origin = &base->sim;
    const char* domainName = sc.stateDependent ? "candidate slots (upper bound before state filtering)" : "strings";
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
    if (startPercent.units && !exactTotal) {
        std::fprintf(stderr, "--start-percent needs an exactly numbered search space (at most 4.6e18 candidates); this pattern uses independent random choices\n"); return 2;
    }
    const uint64_t startIndex = exactTotal ? startPercent.Index(total) : 0;
    auto printStart = [&]() {
        if (!startPercentText.empty()) Say("start position: %llu of %llu (%.9f%%); earlier positions skipped without replay\n",
            (unsigned long long)startIndex, (unsigned long long)total, total ? 100.0 * (double)startIndex / (double)total : 0.0);
    };
    auto saveStart = [&]() {
        if (saveFile.f && !startPercentText.empty()) std::fprintf(saveFile.f, "# start-percent %s\n# start-index %llu\n",
            startPercentText.c_str(), (unsigned long long)startIndex);
    };
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
        if (stochastic && (top == 0 || save) && !exactTotal) {
            std::fprintf(stderr, "--list --stochastic with --top 0 or --save would never end: the pattern has more than 4.6e18 strings\n");
            return 2;
        }
        Say("%.0f %s%s%s\n", sc.rest[0], domainName, sizeNote.c_str(), anyGenerated ? ", * = generated, not stored" : "");
        std::vector<uint64_t> d(sc.segs.size(), 0);
        Seq part;
        Scratch scratch;
        std::unique_ptr<Sim> listState, probe;
        if (sc.stateDependent) listState = std::make_unique<Sim>(), probe = std::make_unique<Sim>();
        BruteChoices brute;
        const Shuffle shuf(exactTotal ? total : 1, seed);
        std::mt19937_64 rng(seed);
        if (saveFile.f && stochastic) std::fprintf(saveFile.f, "# seed %llu\n", (unsigned long long)seed);
        const auto previousInterrupt = std::signal(SIGINT, OnInterrupt);
        printStart();
        saveStart();
        uint64_t listed = 0, excluded = 0;
        for (uint64_t n = startIndex; !g_stop && (save || top == 0 || listed < (uint64_t)top) && !(exactTotal && n >= total); ++n) {
            if (stochastic) digitsOf(exactTotal ? shuf((uint64_t)n) : 0, d, exactTotal ? nullptr : &rng);
            Seq s;
            bool valid = true;
            if (listState) CopySim(*listState, base->sim);
            for (size_t i = 0; i < sc.segs.size(); ++i) {
                if (sc.segs[i].needsState()) {
                    brute.Reset();
                    uint64_t skip;
                    if (!brute.Get(*sc.segs[i].node, d[i], *listState, *probe, part, scratch, skip)) {
                        valid = false;
                        if (!stochastic) {
                            // Every suffix of this unused group is invalid. The
                            // normal odometer below advances directly to skip.
                            d[i] = skip - 1;
                            for (size_t j = i + 1; j < d.size(); ++j) d[j] = sc.segs[j].size() - 1;
                        }
                        break;
                    }
                } else sc.segs[i].Get(d[i], part, scratch);
                if (listState) for (uint8_t in : part) listState->Tick(in);
                s.insert(s.end(), part.begin(), part.end());
            }
            if (valid && sc.excluded && sc.excluded->strings.count(std::string(s.begin(), s.end()))) {
                ++excluded;
                valid = false;
            }
            if (valid) {
                const std::string route = (s.empty() ? "n0" : EncodeInputsRLE(s)) + "\n";
                if (top == 0 || listed < (uint64_t)top) PrintText(route);
                if (saveFile.f) SaveText(saveFile.f, route, saveName);
                ++listed;
            }
            if (stochastic) continue;
            size_t i = sc.segs.size();
            while (i > 0 && ++d[i - 1] == sc.segs[i - 1].size()) d[--i] = 0;
            if (i == 0) break;
        }
        if (saveFile.f) {
            CloseSaved(saveFile.f, saveName);
            std::fprintf(stderr, "saved %llu strings to %s\n", (unsigned long long)listed, saveName.c_str());
        }
        if (g_stop) Say("stopped by Ctrl+C after %llu strings\n", (unsigned long long)listed);
        if (sc.excluded) Say("excluded %llu exact strings from listing\n", (unsigned long long)excluded);
        std::signal(SIGINT, previousInterrupt);
        return 0;
    }

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
    }
    bool shareTaskPrefixes = false;
    if (!stochastic && !mergeReport)
        for (size_t i = 0; i + 1 < sc.split; ++i)
            shareTaskPrefixes |= sc.rest[i + 1] >= PREFIX_MIN_SUFFIXES && sc.segs[i].node->cnt.size() > MIN_SKIP;

    double bruteBytes = 0;
    for (const Segment& s : sc.segs)
        if (s.needsState())
            bruteBytes += 2.0 * (double)std::min(s.node->hi + (s.node->hi < LONG_MAX ? 1 : 0), s.node->bruteHi) * sizeof(BruteChoices::Row);
    const double fixedBytes = (double)threads * ((double)(sc.segs.size() + 3) * sizeof(State) +
                              (sc.stateDependent ? 2.0 : 1.0) * sizeof(Sim) + bruteBytes +
                              2.0 * exclusions.longest) +
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
    // Deterministic task snapshots and the existing per-worker checkpoints share
    // the same 10% memory allowance. Keep stochastic allocation unchanged.
    sc.checkpoints = (size_t)std::min(stochastic ? 256.0 : 16.0 * (double)sc.segs.size(),
                                    spare * (shareTaskPrefixes ? 0.05 : 0.1) / threads / sizeof(State));
    if (mergeReport) mrep = std::make_unique<Report>(), sc.report = mrep.get();
    const double rowShare = std::max(1000.0, spare * 0.3 / ROW_BYTES / threads);
    sc.rowCap = (size_t)(top > 0 ? std::min(rowShare, std::max(10000.0, 4.0 * top)) : rowShare);
    sc.rowByteCap = (size_t)std::max(1.0, spare * 0.3 / threads);
    std::unique_ptr<ResultScratch> saveScratch;
    if (save) saveScratch = std::make_unique<ResultScratch>(saveName), sc.saved = saveScratch.get();
    Best best;
    sc.sampling = stochastic;
    if (stochastic && updateEvery > 0) sc.best = &best;
    std::atomic<uint64_t> nextTask{startIndex}, done{0};
    sc.played = &done;
    std::signal(SIGINT, OnInterrupt);

    Say("level %d: %.0f %s%s, report: %s, %d threads\n", level, sc.rest[0], domainName, sizeNote.c_str(), report.c_str(), threads);
    if (sc.stateDependent)
        Say("b: state-dependent choices; unused slots are skipped. Match counts count visited matching branches, not unexpanded suffixes.\n");
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

    printStart();
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() { return preparationSeconds + std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::atomic<int> running{threads};
    std::atomic<bool> outOfMemory{false};
    std::atomic<bool> timeUp{false};
    std::vector<std::unique_ptr<Walker>> walkers;
    std::vector<std::thread> pool;
    std::vector<std::exception_ptr> saveErrors((size_t)threads);
    if (stochastic) {
        for (uint64_t n = 1; sc.lead + 1 < sc.segs.size() && sc.segs[sc.lead].size() <= sc.checkpoints / n;)
            n *= sc.segs[sc.lead++].size(), sc.leadStates = n;
    }
    std::unique_ptr<DeterministicPrefixes> deterministicPrefixes;
    if (shareTaskPrefixes) {
        const size_t prefixBudget = (size_t)std::max(0.0, spare * 0.1 - (double)threads * sc.checkpoints * sizeof(State));
        if (prefixBudget >= sizeof(DeterministicPrefixes) + DeterministicPrefixes::Cost(0, 0)) {
            deterministicPrefixes = std::make_unique<DeterministicPrefixes>(prefixBudget, (size_t)threads);
            sc.deterministicPrefixes = deterministicPrefixes.get();
        }
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
            if ((exactTotal && c >= total) || (maxSamples && c - startIndex >= maxSamples)) break;
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
            } catch (const SaveError&) {
                saveErrors[(size_t)k] = std::current_exception();
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
                std::snprintf(head, sizeof head, "--- %.0f s: %llu of %.0f %s (%.3g%%), %.0f/s (%.0f/s since the last update)\n%s\n",
                              sec, (unsigned long long)(startIndex + played), sc.rest[0], startIndex ? "positions reached" : sc.stateDependent ? "candidate slots visited" : "strings played",
                              100.0 * (double)(startIndex + played) / sc.rest[0], played / sec,
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
    for (const auto& failure : saveErrors) if (failure) std::rethrow_exception(failure);

    Rows all;
    std::unique_ptr<ResultRuns> savedRuns;
    if (save) savedRuns = std::make_unique<ResultRuns>(*saveScratch, true);
    double sameState = 0;
    uint64_t excludedCount = 0;
    bool evicted = false;
    for (auto& w : walkers) {
        all.matched += w->rows.matched;
        evicted |= w->rows.evicted;
        sameState += w->merged;
        excludedCount += w->excludedCount;
        if (save) {
            w->rows.Flush();
            savedRuns->Take(*w->saved);
        } else for (auto& kv : w->rows.byRoute) all.Add(std::move(kv.second), false);
    }
    walkers.clear();
    Run savedResults;
    if (save) savedResults = SortSavedResults(*savedRuns, sc.rowCap, sc.rowByteCap);
    std::vector<Match> res;
    for (const Match* m : all.order) res.push_back(*m);
    const size_t distinct = save ? (savedResults ? (size_t)savedResults->rows : 0) : all.byRoute.size();
    const char* atLeast = evicted ? "at least " : "";
    if (stochastic) {
        const uint64_t played = done.load();
        Say("%s %llu of %.0f %s (%.3g%%), %.0f match, %s%zu distinct, %.1f s, stopped: %s\n",
                    startIndex ? "position" : sc.stateDependent || sc.excluded ? "visited" : "played", (unsigned long long)(startIndex + played),
                    sc.rest[0], sc.stateDependent ? "candidate slots" : "strings",
                    100.0 * (double)(startIndex + played) / sc.rest[0], all.matched, atLeast, distinct, sec,
                    outOfMemory ? "out of memory" : g_stop ? "Ctrl+C" : timeUp ? "time limit" : (maxSamples && played >= maxSamples) ? "--samples" : sc.stateDependent ? "all candidate slots visited" : sc.excluded ? "all strings visited" : "all strings played");
    } else {
        if (sc.stateDependent)
            Say("%.0f matching branches, %s%zu distinct up to the deciding frame, %.1f s%s\n", all.matched, atLeast,
                distinct, sec, outOfMemory ? "" : g_stop ? " (stopped by Ctrl+C: results so far)" : "");
        else Say("%.0f of %.0f strings match, %s%zu distinct up to the deciding frame, %.1f s%s\n", all.matched, sc.rest[0], atLeast,
                    distinct, sec, outOfMemory ? "" : g_stop ? " (stopped by Ctrl+C: results so far)" : "");
        if (deterministicPrefixes)
            Say("prefix savestates: %llu built, %llu reused, %llu evicted, peak %zu/%zu (minimum %.0fx suffix choices)\n",
                (unsigned long long)deterministicPrefixes->built, (unsigned long long)deterministicPrefixes->reused,
                (unsigned long long)deterministicPrefixes->evicted, deterministicPrefixes->peakEntries,
                deterministicPrefixes->entryLimit, PREFIX_MIN_SUFFIXES);
    }
    if (stochastic && !startPercentText.empty())
        Say("this run: %llu positions visited after skipping %llu; same seed and unchanged inputs preserve the ordering\n",
            (unsigned long long)done.load(), (unsigned long long)startIndex);
    if (sc.excluded) Say("excluded %llu exact strings before simulation\n", (unsigned long long)excludedCount);
    if (hardfilter)
        Say("%.0f %s merged into an earlier one with the same game state (tolerance %g; not counted above)%s\n", sameState,
                    sc.stateDependent ? "candidate slots (upper bound)" : "strings", sc.tol,
                    seen->full ? "; the merge table reached its --maxmem share, later states were not added" : "");
    if (evicted) Say("note: only the best rows were kept (memory share of --maxmem); the strings column may undercount\n");
    if (outOfMemory) Say("stopped early: out of memory (lower --maxmem or --threads); the results above are partial\n");
    if (mrep) mrep->Print();
    if (saveFile.f) {
        saveStart();
        if (stochastic) std::fprintf(saveFile.f, "# seed %llu\n", (unsigned long long)seed);
        SaveText(saveFile.f, distinct ? "frame         px         py         vx         vy   flag   strings\n      inputs\n" : "# no string matched\n", saveName);
        if (savedResults) {
            ResultFile input(savedResults->path, "rb");
            Match m;
            while (input.Read(m)) SaveText(saveFile.f, RowText(m, true, true), saveName);
        }
        CloseSaved(saveFile.f, saveName);
        saveScratch->clean = true;
        std::fprintf(stderr, "saved %zu rows to %s\n", distinct, saveName.c_str());
    }
    if (!distinct) return 1;
    Say("frame         px         py         vx         vy   flag   strings  inputs\n");
    std::string rows;
    auto show = [&](const Match& m) {
        rows += RowText(m, true);
        if (rows.size() >= 65536) PrintText(rows), rows.clear();
    };
    if (savedResults) {
        ResultFile input(savedResults->path, "rb");
        Match m;
        for (size_t i = 0; (top == 0 || i < (size_t)top) && input.Read(m); ++i) show(m);
    } else for (size_t i = 0; i < res.size() && (top == 0 || i < (size_t)top); ++i) show(res[i]);
    PrintText(rows);
    if (top && distinct > (size_t)top) Say("... %s%zu more (--top 0 shows all)\n", atLeast, distinct - (size_t)top);
    return 0;
}

// rbview uses the same graceful stop path as Ctrl+C, from its worker's event thread.
// This does not alter search enumeration, state comparison or simulation.
void RequestSmartforceStop() { g_stop.store(true, std::memory_order_relaxed); }

int CmdSmartforce(int argc, char** argv) {
    try {
        return Smartforce(argc, argv);
    } catch (const std::bad_alloc&) {
        std::fflush(stdout);
        std::fprintf(stderr, "out of memory: lower --maxmem or --threads\n");
        return 3;
    } catch (const SaveError& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "--save: %s; results are incomplete\n", e.what());
        return 3;
    }
}
