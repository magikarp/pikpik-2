#pragma once
#include <stdint.h>

// ResTIMG remains a 32-byte disk-compatible header. Relocated texture copies
// retain their host pointers separately instead of truncating pointer deltas.
void* p2_texture_image(const void* header, uint32_t offset);
void* p2_texture_palette(const void* header, uint32_t offset);
void p2_texture_copy(const void* destination, const void* source,
                     uint32_t imageOffset, uint32_t paletteOffset);
void p2_texture_forget(const void* header);
