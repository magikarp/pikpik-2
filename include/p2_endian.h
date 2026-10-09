#pragma once
#include <cstddef>
#include <cstring>
#include <type_traits>

// Symmetric conversion between a scalar's native bytes and GameCube bytes.
// Raw buffers are intentionally not converted: their field layout is separate.
template <typename T> inline T p2_big_endian(T value) {
    static_assert(std::is_arithmetic<T>::value, "Only scalar values have an endian conversion");
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned char input[sizeof(T)], output[sizeof(T)];
    std::memcpy(input, &value, sizeof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i) output[i] = input[sizeof(T)-1-i];
    std::memcpy(&value, output, sizeof(T));
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error Unsupported host byte order
#endif
    return value;
}

template <typename T> inline T p2_read_big(const void* bytes) {
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return p2_big_endian(value);
}

// A scalar stored in a disk structure. Size and alignment remain those of T;
// conversion happens on access, including after a raw stream read.
template <typename T> struct P2Big {
    T encoded;
    P2Big() = default;
    P2Big(T value) : encoded(p2_big_endian(value)) {}
    operator T() const { return p2_big_endian(encoded); }
    P2Big& operator=(T value) { encoded = p2_big_endian(value); return *this; }
};
static_assert(sizeof(P2Big<unsigned int>) == 4, "GameCube word layout");
template <typename T> struct P2BigVec3 { P2Big<T> x,y,z; };
static_assert(std::is_trivially_copyable<P2Big<unsigned int>>::value, "Raw resource reads must remain valid");
