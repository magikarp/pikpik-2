#pragma once
#include <cstddef>
struct J3DVertexData;
struct J3DVertexBlock;
bool p2_load_vertex(J3DVertexData* output, const J3DVertexBlock* block, std::size_t available);
