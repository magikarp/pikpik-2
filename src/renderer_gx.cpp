// Remaining portable GX state setup used by J3D and JPA. This translation
// unit uses Aurora's SDK/shadow-state layout, never the game's __GXData.
#include <dolphin/gx.h>
#include "dolphin/gx/__gx.h"
#include "gx/fifo.hpp"
#include <cstdlib>
#include <cstring>

extern "C" void GXInitTexCacheRegion(GXTexRegion* region, GXBool mipmap32,
                                     u32 evenAddress, GXTexCacheSize evenSize,
                                     u32 oddAddress, GXTexCacheSize oddSize) {
    // SDK cache descriptors are two BP register payloads followed by flags.
    // Aurora owns real texture storage; these console TMEM values are metadata.
    static_assert(sizeof(GXTexRegion)==16, "GX cache descriptor ABI");
    if(!region || static_cast<unsigned>(evenSize)>GX_TEXCACHE_512K || static_cast<unsigned>(oddSize)>GX_TEXCACHE_NONE) std::abort();
    const u32 evenCode=static_cast<u32>(evenSize)+3;
    const u32 oddCode=oddSize==GX_TEXCACHE_NONE ? 0 : static_cast<u32>(oddSize)+3;
    const u32 even=((evenAddress>>5)&0x7fff)|(evenCode<<15)|(evenCode<<18);
    const u32 odd=((oddAddress>>5)&0x7fff)|(oddCode<<15)|(oddCode<<18);
    auto* bytes=reinterpret_cast<unsigned char*>(region);
    std::memcpy(bytes,&even,sizeof(even));
    std::memcpy(bytes+4,&odd,sizeof(odd));
    bytes[12]=mipmap32;
    bytes[13]=1;
}

extern "C" void GXSetMisc(GXMiscToken token,u32 value) {
    switch(token) {
    case GX_MT_NULL: return;
    case GX_MT_XF_FLUSH:
        __gx->vNum=static_cast<u16>(value);
        __gx->bpSent=0;
        if(__gx->vNum) __gx->dirtyState|=1u<<3;
        // Aurora's __GXSendFlushPrim consumes this state without emitting the
        // console's dummy triangle strip. Native draws apply BP state directly.
        return;
    case GX_MT_DL_SAVE_CONTEXT:
        __gx->dlSaveContext=value!=0;
        return;
    default: std::abort(); // Do not silently accept unimplemented hardware modes.
    }
}

extern "C" void GXSetCopyClamp(GXFBClamp clamp) {
    // Keep the two SDK clamp bits in texture-copy commands. Aurora resolves
    // EFB rectangles directly and presents with a ClampToEdge render sampler;
    // it has no display-copy shadow register or console vertical-copy filter.
    // JFWDisplay requests top+bottom clamping. Console filter differences for
    // the other modes are not emulated by this native presentation path.
    const u32 bits=static_cast<u32>(clamp);
    if(bits&~3u) std::abort();
    __gx->cpTex=(__gx->cpTex&~3u)|bits;
}

extern "C" void GXWaitDrawDone() {
    // Pairs with GXSetDrawDone, which queued the draw-done token and published
    // the FIFO. Waiting for the FIFO to drain is Aurora's GXDrawDone wait.
    aurora::gx::fifo::drain();
}

extern "C" void __GXSetIndirectMask(u32 mask) {
    // SDK BP register 0x0F (indirect texture-map mask). Aurora resolves
    // indirect lookups itself and ignores the register; keep the command.
    GX_WRITE_RAS_REG((0x0Fu << 24) | (mask & 0xFFu));
    __gx->bpSent = 1;
}
