#pragma once
#include <cstddef>
#include <cstdint>

// Decode one bounded THP JPEG payload into GX I8 tiled planes. Dimensions
// must agree with the movie header; all output capacities are explicit.
extern "C" std::int32_t p2_thp_decode(const void* input, std::size_t inputSize,
    void* y, std::size_t ySize, void* u, std::size_t uSize,
    void* v, std::size_t vSize, std::uint32_t width, std::uint32_t height);
