#pragma once
#include <cstddef>
// J2D texture/font reference table (count, offsets, typed length-prefixed names).
bool p2_validate_2d_references(const void* data, std::size_t size);
