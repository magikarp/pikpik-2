#pragma once
#include "p2_endian.h"
#include <cstdint>

// Validate archive-owned bytes before the original pointer-only JMessage parser.
inline bool p2_validate_message_resource(const void* data, std::size_t extent, bool color) {
    using U16 = std::uint16_t;
    using U32 = std::uint32_t;
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (!bytes || extent < 32 || std::memcmp(bytes, color ? "MGCLbmc1" : "MESGbmg1", 8)) return false;
    const std::size_t size = p2_read_big<U32>(bytes + 8);
    if (size < 32 || size > extent || (!color && bytes[16] > 3)) return false;
    const unsigned char *info = nullptr, *messages = nullptr, *ids = nullptr;
    std::size_t messageSize = 0, at = 32;
    bool colors = false;
    for (U32 n = p2_read_big<U32>(bytes + 12); n; --n) {
        if (size - at < 8) return false;
        const auto* block = bytes + at;
        const std::size_t length = p2_read_big<U32>(block + 4);
        if (length < 8 || length > size - at) return false;
        if (!std::memcmp(block, "INF1", 4)) {
            if (color || info || length < 16) return false;
            const auto count = p2_read_big<U16>(block + 8), stride = p2_read_big<U16>(block + 10);
            if (stride < 4 || std::size_t(count) * stride > length - 16) return false;
            info = block;
        } else if (!std::memcmp(block, "DAT1", 4)) {
            if (color || messages || !info) return false;
            messages = block + 8; messageSize = length - 8;
        } else if (!std::memcmp(block, "MID1", 4)) {
            if (color || ids || length < 16 || block[11] > 4) return false;
            if (std::size_t(p2_read_big<U16>(block + 8)) * 4 > length - 16) return false;
            ids = block;
        } else if (!std::memcmp(block, "CLT1", 4)) {
            if (!color || colors || length < 12) return false;
            colors = true;
        } else if (color || std::memcmp(block, "STR1", 4)) return false;
        at += length;
    }
    if (at != size) return false;
    if (color) return colors;
    if (!info || !messages) return false;
    const auto count = p2_read_big<U16>(info + 8), stride = p2_read_big<U16>(info + 10);
    if (ids && p2_read_big<U16>(ids + 8) != count) return false;
    for (unsigned i = 0; i < count; ++i) {
        std::size_t pos = p2_read_big<U32>(info + 16 + i * stride);
        for (;;) {
            if (pos >= messageSize) return false;
            if (!messages[pos]) break;
            if (messages[pos] == 0x1a) {
                if (messageSize - pos < 2) return false;
                const auto length = messages[pos + 1];
                if (length < 5 || length > messageSize - pos) return false;
                pos += length;
            } else ++pos;
        }
    }
    return true;
}
