#include "Dolphin/gba.h"
#include "ebi/CardEReader.h"
#include <cstdio>
#include <cstring>

int main() {
    GBAInit();
    GBAInit();
    const u8 original[8] = {0x50, 0x49, 0x4b, 0x49, 0x12, 0x34, 0x56, 0x78};
    for (s32 channel = 0; channel < 4; ++channel) {
        u8 payload[8];
        std::memcpy(payload, original, sizeof(payload));
        // Run the game's actual handshake/upload code: no payload is sent,
        // no successful connection is fabricated, and it must not wait.
        if (ebi::CardEReader::CardE_uploadToGBA(channel, payload, sizeof(payload)) ||
            std::memcmp(payload, original, sizeof(payload))) {
            std::fprintf(stderr, "e-Reader upload did not reject absent GBA on port %d\n", channel);
            return 1;
        }
        u8 status = 0xa5;
        if (GBAGetStatus(channel, &status) != 1 || GBARead(channel, payload, &status) != 1 ||
            GBAWrite(channel, payload, &status) != 1 || status != 0xa5 ||
            std::memcmp(payload, original, sizeof(payload))) {
            std::fputs("GBA failure must preserve output buffers\n", stderr);
            return 1;
        }
    }
    std::puts("Original e-Reader upload rejects all four disconnected ports without modifying payloads.");
    return 0;
}
