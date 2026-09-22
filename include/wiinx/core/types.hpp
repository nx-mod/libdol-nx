// Basic types, and reading/writing the Wii's big-endian memory.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace wiinx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

// An address in the game's 32-bit memory - never a host pointer.
using GuestAddr = u32;

namespace detail {
inline u8 bswap(u8 v) noexcept { return v; }
inline u16 bswap(u16 v) noexcept { return __builtin_bswap16(v); }
inline u32 bswap(u32 v) noexcept { return __builtin_bswap32(v); }
inline u64 bswap(u64 v) noexcept { return __builtin_bswap64(v); }

template <class T>
using RawOf = std::conditional_t<sizeof(T) == 1, u8,
              std::conditional_t<sizeof(T) == 2, u16,
              std::conditional_t<sizeof(T) == 4, u32, u64>>>;
}  // namespace detail

// Big-endian bytes -> a host value (integers and floats).
template <class T>
T from_be(const u8* p) noexcept {
    static_assert(std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 ||
                                                      sizeof(T) == 4 || sizeof(T) == 8));
    detail::RawOf<T> raw;
    std::memcpy(&raw, p, sizeof(raw));
    raw = detail::bswap(raw);
    T value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// A host value -> big-endian bytes.
template <class T>
void to_be(u8* p, T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 ||
                                                      sizeof(T) == 4 || sizeof(T) == 8));
    detail::RawOf<T> raw;
    std::memcpy(&raw, &value, sizeof(raw));
    raw = detail::bswap(raw);
    std::memcpy(p, &raw, sizeof(raw));
}

}  // namespace wiinx
