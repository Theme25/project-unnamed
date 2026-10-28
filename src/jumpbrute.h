#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rb {

// Candidate placement from the Practice Hack's BruteForcer.as / BruteEnv.
// A marked interval has `frames` inputs. vy is sampled after its first native
// Tick, just as Session::BruteStart does. The last input cannot affect position
// at the marked endpoint, so the weighted indices are frames-1 down to 1.
// This is a clear-flight candidate model; always replay the inputs natively.
struct JumpBrute {
    static constexpr double PI = 3.141592653589793;
    int steps;
    double vy;

    JumpBrute(int frames, double initialVy) : steps(frames - 1), vy(initialVy) {}

    int MaxCount() const {
        if (steps < 0 || !std::isfinite(vy)) return -1;
        const double limit = std::ceil(vy / (40 / (147 * PI) - 1.0 / 3));
        return (int)std::max(-1.0, std::min((double)steps, limit));
    }
    int Window(int count) const {
        if (!count) return 0;
        const double last = std::ceil(-vy * 3 + (count - 1) * 40 / (49 * PI));
        return (int)std::max(0.0, std::min((double)steps, last));
    }
    uint64_t Maximum(int count) const {
        return (uint64_t)count * (uint64_t)(2LL * steps - count + 1) / 2;
    }
    uint64_t Minimum(int count) const {
        return (uint64_t)count * (uint64_t)(2LL * (steps - Window(count)) + count + 1) / 2;
    }
    bool Allows(int count, uint64_t sum) const {
        return count >= 0 && count <= MaxCount() && Window(count) >= count &&
               sum >= Minimum(count) && sum <= Maximum(count);
    }
    template <class F> void Place(int count, uint64_t sum, F&& press) const {
        if (!count) return;
        const int window = Window(count), shift = window - count;
        if (shift == 0) {
            for (int i = 0; i < count; ++i) press(i);
            return;
        }
        const uint64_t below = Maximum(count) - sum;
        const int end = (int)(below / (uint64_t)shift), middle = (int)(below % (uint64_t)shift);
        const int first = count - end - 1;
        for (int i = 0; i < first; ++i) press(i);
        if (end < count) press(first + middle);
        for (int i = window - end; i < window; ++i) press(i);
    }
};

} // namespace rb
