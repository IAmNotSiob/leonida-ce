// Clean-room BCPack (DirectStorage compression format 14) decoder: C++17 port of tools/bcpack.py.
//
// Strict mode only (bcpack.decode(raw, codec, out_size) with allow_provisional=False, history=None):
//   - BC1/BC3/BC4/BC5 use the confirmed built-in history entries shipped in tools/bcpack_tables.py;
//   - BC6H/BC7 decode only self-contained chunks (no reference to the unpublished initial history).
// Anything the Python reference would refuse (stream error, unconfirmed/unknown history, BC7 inferred
// signature, consistency check) makes these functions return false without touching dst, so the caller can
// fall back (e.g. zero-fill). They never throw and are thread-safe (tables are immutable after first use).
//
// The output is the chunk's BCn block stream in the surface's tiled storage order (as the console's
// hardware decoder writes it); no detiling is done here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bcpack {

// DSTORAGE_BCPACK_MODE values (1 = BC1 ... 7 = BC7). BC2 (2) does not exist in the corpus and is unsupported.
enum Mode : int { kAuto = 0, kBC1 = 1, kBC3 = 3, kBC4 = 4, kBC5 = 5, kBC6H = 6, kBC7 = 7 };

// Decodes one BCPack chunk of `src_len` bytes into exactly `dst_len` bytes of BCn blocks.
// `mode` = 1,3,4,5,6,7, or kAuto (0) to try the modes in order BC1, BC7, BC5, BC6H, BC4, BC3 and keep the first
// one whose stream validates (the rANS layer checks every fragment's end state, so a wrong mode is refused).
// `src` may include trailing zero padding (as stored in the pool). On success returns true and, if
// `mode_used` is non-null, stores the mode that decoded. On failure returns false, leaves dst untouched and
// puts a reason in *why (if non-null).
bool decode(const uint8_t* src, size_t src_len, int mode, uint8_t* dst, size_t dst_len, std::string* why = nullptr,
            int* mode_used = nullptr);

namespace testing {
// Verification only, never for the runtime: BC6H/BC7 references to the (unpublished) initial history read zeros and
// BC6H anchor / BC7 anchor+p-bit consistency failures are ignored, so the block rebuilders can be compared
// against tools/bcpack.py on real chunks. Output is NOT a correct texture.
bool decode_zero_history(const uint8_t* src, size_t src_len, int mode, uint8_t* dst, size_t dst_len, std::string* why);
}  // namespace testing

}  // namespace bcpack

// Convenience C-style entry point: auto-detected mode.
inline bool bcpack_decode(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* why) {
    return bcpack::decode(src, src_len, bcpack::kAuto, dst, dst_len, why);
}
