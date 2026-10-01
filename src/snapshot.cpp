// snapshot.cpp - see snapshot.h
#include "snapshot.h"
#include <cstring>
#include <type_traits>

namespace rb {

static_assert(std::is_trivially_copyable<Sim>::value, "compact snapshots copy Sim as raw bytes");

namespace {
constexpr size_t BLOCK = 8;
constexpr size_t NBLOCKS = (sizeof(Sim) + BLOCK - 1) / BLOCK;

inline bool BlockDiffers(const unsigned char* a, const unsigned char* b, size_t blk) {
    const size_t off = blk * BLOCK, n = off + BLOCK <= sizeof(Sim) ? BLOCK : sizeof(Sim) - off;
    return std::memcmp(a + off, b + off, n) != 0;
}
inline void Put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xff));
    out.push_back((uint8_t)(v >> 8));
}
inline uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
}  // namespace

size_t EncodeSnapshot(const Sim& base, const Sim& s, std::vector<uint8_t>& out) {
    const unsigned char* a = reinterpret_cast<const unsigned char*>(&base);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&s);
    const size_t start = out.size();
    out.resize(start + 4);  // run count, filled in at the end
    uint32_t runs = 0;
    size_t blk = 0;
    while (blk < NBLOCKS) {
        // skip equal 64-byte chunks quickly
        if (blk % 8 == 0 && (blk + 8) * BLOCK <= sizeof(Sim) && std::memcmp(a + blk * BLOCK, b + blk * BLOCK, 8 * BLOCK) == 0) {
            blk += 8;
            continue;
        }
        if (!BlockDiffers(a, b, blk)) {
            ++blk;
            continue;
        }
        size_t end = blk + 1;
        while (end < NBLOCKS && end - blk < 65535 && BlockDiffers(a, b, end)) ++end;
        Put16(out, (uint16_t)blk);
        Put16(out, (uint16_t)(end - blk));
        const size_t off = blk * BLOCK, n = std::min(end * BLOCK, sizeof(Sim)) - off;
        out.insert(out.end(), b + off, b + off + n);
        ++runs;
        blk = end;
    }
    std::memcpy(&out[start], &runs, 4);
    return out.size() - start;
}

size_t DecodeSnapshot(const Sim& base, const uint8_t* data, Sim& out) {
    std::memcpy(static_cast<void*>(&out), &base, sizeof(Sim));
    unsigned char* o = reinterpret_cast<unsigned char*>(&out);
    uint32_t runs;
    std::memcpy(&runs, data, 4);
    const uint8_t* p = data + 4;
    for (uint32_t r = 0; r < runs; ++r) {
        const size_t blk = Get16(p), cnt = Get16(p + 2);
        p += 4;
        const size_t off = blk * BLOCK, n = std::min((blk + cnt) * BLOCK, sizeof(Sim)) - off;
        std::memcpy(o + off, p, n);
        p += n;
    }
    return (size_t)(p - data);
}

}  // namespace rb
