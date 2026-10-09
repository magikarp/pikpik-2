#pragma once
#include <cstddef>
#include <cstdint>
struct J3DEnvelopeBlock;
struct P2EnvelopeData {
    uint16_t count;
    uint32_t influences;
    uint32_t matrices;
    uint8_t* counts;
    uint16_t* indices;
    float* weights;
    float (*inverseBind)[3][4];
};
P2EnvelopeData* p2_load_envelope(const J3DEnvelopeBlock* block, std::size_t available);
