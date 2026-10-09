#pragma once
#include <cstddef>
struct J3DShapeBlock;
struct J3DShapeFactory;
// Owns host-order shape tables; primitive display-list bytes stay big-endian.
bool p2_load_shape(J3DShapeFactory*, const J3DShapeBlock*, std::size_t available);
