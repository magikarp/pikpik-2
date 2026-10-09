#pragma once
#include <cstddef>
struct J3DMaterialBlock;
// Caller owns the returned MAT3 copy and releases it with p2_game_free.
// The block header retains disk offsets; factory payload tables are host order.
void* p2_decode_material(const J3DMaterialBlock*, std::size_t available);
struct J2DMaterialBlock;
// MAT1 equivalent; caller owns the converted payload copy.
void* p2_decode_2d_material(const J2DMaterialBlock*, std::size_t available);
