#pragma once
#ifdef __cplusplus
// This header is also included from the SDK's extern-C block.
extern "C++" {
#include <cstdint>
#include <cstring>
extern "C" void p2_gx_write_bytes(const void* data, uint32_t size);
extern "C" void p2_gx_array_base(uint32_t attribute, const void* data, uint32_t size, bool littleEndian);
template<class T> struct P2FifoField {
    T operator=(T value) const {
        unsigned char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        for (unsigned i=0; i<sizeof(T)/2; ++i) {
            const unsigned char tmp = bytes[i];
            bytes[i] = bytes[sizeof(T)-1-i]; bytes[sizeof(T)-1-i] = tmp;
        }
#endif
        p2_gx_write_bytes(bytes, sizeof(T));
        return value;
    }
};
struct P2Fifo {
    P2FifoField<uint8_t> u8;
    P2FifoField<uint16_t> u16;
    P2FifoField<uint32_t> u32;
    P2FifoField<uint64_t> u64;
    P2FifoField<int8_t> s8;
    P2FifoField<int16_t> s16;
    P2FifoField<int32_t> s32;
    P2FifoField<int64_t> s64;
    P2FifoField<float> f32;
    P2FifoField<double> f64;
};
extern const P2Fifo p2_gx_fifo;
}
#endif
