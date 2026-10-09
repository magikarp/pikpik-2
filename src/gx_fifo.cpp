#include "p2_gx_fifo.h"
#include "gx/fifo.hpp"
#include <dolphin/gx.h>
const P2Fifo p2_gx_fifo{};
extern "C" void p2_gx_write_bytes(const void* data, uint32_t size) {
    aurora::gx::fifo::write_data(data, size);
}
extern "C" void p2_gx_array_base(uint32_t attribute, const void* data, uint32_t size, bool littleEndian) {
    const uint32_t index=attribute==GX_VA_NBT ? 1 : attribute-GX_VA_POS;
    if(index>15 || (!data && size)) std::abort();
    using namespace aurora::gx::fifo;
    write_u8(GX_AURORA);
    write_u16(GX_AURORA_LOAD_ARRAYBASE+index);
    write_u64(reinterpret_cast<uintptr_t>(data));
    write_u32(size);
    write_u8(littleEndian ? 1 : 0);
}

// These are diagnostic FIFO-consumption callbacks, matching Aurora's existing
// draw-done boundary. They do not claim that Metal has finished GPU execution.
extern "C" GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback callback) {
    return aurora::gx::fifo::set_draw_sync_callback(callback);
}
extern "C" void GXSetDrawSync(u16 token) {
    using namespace aurora::gx::fifo;
    write_u8(GX_LOAD_BP_REG); write_u32(0x48000000u | token);
    write_u8(GX_LOAD_BP_REG); write_u32(0x47000000u | token);
    GXFlush();
    publish();
}
