// snapshot.h - compact, bit-exact snapshots of a Sim.
//
// A full Sim is ~152 KB of fixed-capacity arrays, most of which are empty slots or data that
// never changes after the level is built (shape geometry, static bodies, joint definitions).
// A compact snapshot stores only the 8-byte blocks that differ from a BASE state (normally the
// freshly loaded level): restoring copies the base and patches those blocks back in, so the
// result is byte-for-byte the original state, including stale data in unused slots that the
// engine may read. Bit-exactness therefore holds by construction, whatever the engine does.
//
// Typical sizes (8-byte blocks, vs. the level start): Level 2 ~1.4 KB, Level 8 ~9 KB average,
// 16 KB worst along the TAS.
//
// Portability: the only pointers inside Sim (template, geometry table) are equal to the base's
// in the encoding process and are therefore never stored; decoding against a base built by
// another process (same binary, same level/checkpoint) gives that process's pointers. Compact
// snapshots can thus be written to disk and resumed later.
//
// Format: uint32 runCount, then per run: uint16 blockOffset, uint16 blockCount, blockCount * 8
// bytes. (sizeof(Sim) / 8 < 65536 is checked at compile time.)
#pragma once
#include "redball.h"
#include <cstdint>
#include <vector>

namespace rb {

static_assert((sizeof(Sim) + 7) / 8 < 65536, "block offsets are 16-bit");

// Appends the compact encoding of `s` relative to `base` to `out`; returns the encoded size.
size_t EncodeSnapshot(const Sim& base, const Sim& s, std::vector<uint8_t>& out);
// Rebuilds the state encoded at `data` (relative to the same base) into `out`; returns bytes read.
size_t DecodeSnapshot(const Sim& base, const uint8_t* data, Sim& out);

}  // namespace rb
