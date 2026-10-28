#pragma once
#include <cstdint>
#include <string>

namespace rb {
// Exact decimal percentages: nine fractional digits, no floating-point rounding
// of large candidate counts. A trailing percent sign is optional.
struct SearchOffset {
    static constexpr uint64_t scale = 1000000000ULL;
    uint64_t units = 0;
    template<class Char> bool Parse(std::basic_string<Char> text) {
        units = 0;
        if (!text.empty() && text.back() == Char('%')) text.pop_back();
        uint64_t whole = 0, fraction = 0, place = scale;
        bool dot = false, digit = false;
        for (Char c : text) {
            if (c == Char('.') && !dot) { dot = true; continue; }
            if (c < Char('0') || c > Char('9')) return false;
            digit = true;
            if (!dot) {
                whole = whole * 10 + uint64_t(c - Char('0'));
                if (whole > 100) return false;
            } else {
                if (place == 1) return false;
                place /= 10;
                fraction += uint64_t(c - Char('0')) * place;
            }
        }
        if (!digit || (whole == 100 && fraction)) return false;
        units = whole * scale + fraction;
        return true;
    }
    uint64_t Index(uint64_t total) const {
        return (uint64_t)((__uint128_t)total * units / (100 * scale));
    }
};
} // namespace rb
