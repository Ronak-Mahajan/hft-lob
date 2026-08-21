#pragma once
// ---------------------------------------------------------------------------
// lob/common.hpp — platform shims: byte order, timestamps, branch hints.
// C++20, header-only. No allocation, no exceptions on any path in this file.
// ---------------------------------------------------------------------------
#include <bit>
#include <cstdint>
#include <cstdlib>

#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
#endif

namespace lob {

#if defined(__GNUC__) || defined(__clang__)
  #define LOB_LIKELY(x)   (__builtin_expect(!!(x), 1))
  #define LOB_UNLIKELY(x) (__builtin_expect(!!(x), 0))
  #define LOB_FORCE_INLINE inline __attribute__((always_inline))
#else
  #define LOB_LIKELY(x)   (x)
  #define LOB_UNLIKELY(x) (x)
  #define LOB_FORCE_INLINE __forceinline
#endif

inline constexpr uint32_t NIL = 0xFFFFFFFFu;   // null pool/level index

// --- big-endian → host (ITCH is big-endian; x86 is little-endian) ----------
LOB_FORCE_INLINE uint16_t be16(uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}
LOB_FORCE_INLINE uint32_t be32(uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
LOB_FORCE_INLINE uint64_t be64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}
static_assert(std::endian::native == std::endian::little,
              "big-endian hosts need identity shims here");

// --- cycle counter ----------------------------------------------------------
// rdtsc: ~7ns read, constant-rate (invariant TSC) on anything post-Nehalem.
// rdtscp additionally serializes prior loads/stores — use it for the *end*
// timestamp so the measured op cannot drift past the read.
#if defined(__x86_64__) || defined(_M_X64)
LOB_FORCE_INLINE uint64_t rdtsc_begin() {
    _mm_lfence();                 // fence: don't let the op start early
    return __rdtsc();
}
LOB_FORCE_INLINE uint64_t rdtsc_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);  // waits for prior instructions to retire
    _mm_lfence();
    return t;
}
#else
#include <chrono>
LOB_FORCE_INLINE uint64_t rdtsc_begin() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
LOB_FORCE_INLINE uint64_t rdtsc_end() { return rdtsc_begin(); }
#endif

// Fatal invariant violation. Kept out-of-line and cold; the checks that call
// it compile to a single predictable branch on the hot path.
[[noreturn]] inline void die(const char* /*msg*/) { std::abort(); }

#define LOB_ASSERT(cond, msg) do { if (LOB_UNLIKELY(!(cond))) ::lob::die(msg); } while (0)

} // namespace lob
