#include "p2_gx_fifo.h"
#include "gx/fifo.hpp"
#include "gx/regs.hpp"
#include <dolphin/os.h>
#include <aurora/aurora.h>
namespace aurora { extern AuroraConfig g_config; }
extern void* MEM1Start;
extern void* MEM1End;
#include "dolphin/gx/__gx.h"
#include <dolphin/gx.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
extern "C" void GXSetDrawSync(u16);
namespace {
std::atomic<unsigned> tokenCount{0};
u16 tokens[132]{};
std::thread::id callbackThread;
void recordToken(u16 token) {
    callbackThread=std::this_thread::get_id();
    const unsigned index=tokenCount.load();
    if(index<132) tokens[index]=token;
    tokenCount.store(index+1,std::memory_order_release);
}
}

int main() {
    // Retail code may omit GXEnd for fixed-count draws, including the clear
    // quad followed by the boot fader. State commands between them are valid.
    alignas(32) unsigned char implicitDraws[256]{};
    __gx->dirtyState=0; __gx->vNum=0; __gx->bpSent=0;
    aurora::gx::fifo::begin_display_list(implicitDraws,sizeof(implicitDraws));
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(unsigned i=0;i<4;++i) { p2_gx_fifo.u16=i; p2_gx_fifo.u16=i+1; }
    // Complete BP state command following the 16-byte vertex payload.
    p2_gx_fifo.u8=GX_LOAD_BP_REG; p2_gx_fifo.u32=0x40000000;
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(unsigned i=0;i<4;++i) { p2_gx_fifo.u16=i+2; p2_gx_fifo.u16=i+3; }
    GXEnd();
    const auto implicitLength=aurora::gx::fifo::end_display_list();
    if(implicitLength!=64 || implicitDraws[0]!=GX_QUADS || implicitDraws[2]!=4 ||
       implicitDraws[19]!=GX_LOAD_BP_REG || implicitDraws[24]!=GX_QUADS || implicitDraws[26]!=4) {
        std::fputs("Implicit fixed-count GXEnd changed command boundaries\n",stderr); return 1;
    }
    unsigned char bytes[64];
    std::memset(bytes,0xcc,sizeof(bytes));
    aurora::gx::fifo::begin_display_list(bytes, sizeof(bytes));
    p2_gx_fifo.u8 = 0x08; // CP register load
    p2_gx_fifo.u8 = 0xa0;
    p2_gx_fifo.u32 = 0x00123456;
    p2_gx_fifo.f32 = 1.0f;
    p2_gx_fifo.s16 = -2;
    const unsigned char expected[] = {8, 0xa0, 0, 0x12, 0x34, 0x56, 0x3f, 0x80, 0, 0, 0xff, 0xfe};
    const auto length = aurora::gx::fifo::end_display_list();
    const unsigned char padding[32]={};
    if (length != 32 || std::memcmp(bytes, expected, sizeof(expected)) ||
        std::memcmp(bytes+sizeof(expected),padding,32-sizeof(expected)) || bytes[32]!=0xcc) {
        std::fprintf(stderr, "Game FIFO bridge emitted incorrect display-list bytes\n"); return 1;
    }
    float vertices[9] = {};
    aurora::gx::fifo::begin_display_list(bytes, sizeof(bytes));
    p2_gx_array_base(GX_VA_POS, vertices, sizeof(vertices), true);
    const auto arrayLength = aurora::gx::fifo::end_display_list();
    uint64_t address = 0;
    for (unsigned i=3; i<11; ++i) address=(address<<8)|bytes[i];
    const uint32_t size=(uint32_t(bytes[11])<<24)|(uint32_t(bytes[12])<<16)|(uint32_t(bytes[13])<<8)|bytes[14];
    if (arrayLength != 32 || bytes[0] != GX_AURORA || bytes[1] != 0 || bytes[2] != GX_AURORA_LOAD_ARRAYBASE ||
        address != reinterpret_cast<uintptr_t>(vertices) || size != sizeof(vertices) || bytes[15] != 1) {
        std::fprintf(stderr, "Native array command lost its pointer, bounds, or byte order\n"); return 1;
    }
    if(std::memcmp(bytes+16,padding,16) || bytes[32]!=0xcc) {
        std::fprintf(stderr,"Display-list padding corrupted array command bounds\n"); return 1;
    }
    // J3D uses both TMEM interleavings and three cache sizes. Reserved bytes
    // stay untouched; odd-cache NONE has a distinct zero size encoding.
    for(unsigned cache=0;cache<3;++cache) for(unsigned odd=0;odd<4;++odd) for(unsigned bank=0;bank<2;++bank) {
        GXTexRegion region;std::memset(&region,0xa5,sizeof(region));
        const u32 evenAddress=bank?0:0x80000,oddAddress=bank?0x80000:0;
        GXInitTexCacheRegion(&region,GX_TRUE,evenAddress,static_cast<GXTexCacheSize>(cache),
                             oddAddress,static_cast<GXTexCacheSize>(odd));
        u32 words[4];std::memcpy(words,&region,sizeof(words));
        const auto* flags=reinterpret_cast<const unsigned char*>(&region);
        const unsigned oddCode=odd==3?0:odd+3;
        if((words[0]&0x7fff)!=(bank?0:0x4000)||((words[0]>>15)&7)!=cache+3||
           ((words[0]>>18)&7)!=cache+3||(words[1]&0x7fff)!=(bank?0x4000:0)||
           ((words[1]>>15)&7)!=oddCode||((words[1]>>18)&7)!=oddCode||words[2]!=0xa5a5a5a5||
           flags[12]!=1||flags[13]!=1||flags[14]!=0xa5||flags[15]!=0xa5) {
            std::fputs("Texture cache descriptor differs from SDK layout\n",stderr);return 1;
        }
    }
    // Check that the real GXCopyTex encoder retains clamp bits and neighboring
    // shadow state. These recorded commands are not submitted for GPU copies.
    const u32 previousCopy=__gx->cpTex;
    for(unsigned clamp=0;clamp<4;++clamp) {
        __gx->cpTex=0xc0;
        GXSetCopyClamp(static_cast<GXFBClamp>(clamp));
        aurora::gx::fifo::begin_display_list(bytes,sizeof(bytes));
        GXCopyTex(vertices,GX_FALSE);
        const auto copyLength=aurora::gx::fifo::end_display_list();
        if(copyLength!=32||bytes[11]!=GX_LOAD_BP_REG||bytes[12]!=0x52||bytes[15]!=(0xc0|clamp)) {
            std::fputs("Texture-copy command lost clamp or adjacent state bits\n",stderr);return 1;
        }
    }
    __gx->cpTex=previousCopy;
    // Exercise Aurora's real display-list save/restore, not just field writes.
    GXSetMisc(GX_MT_DL_SAVE_CONTEXT,1);
    unsigned char contextBytes[512];
    GXBeginDisplayList(contextBytes,sizeof(contextBytes));
    GXSetMisc(GX_MT_XF_FLUSH,8);
    GXEndDisplayList();
    if(__gx->vNum!=0||__gx->dirtyState!=0) return 1;
    GXSetMisc(GX_MT_DL_SAVE_CONTEXT,0);
    GXSetMisc(GX_MT_XF_FLUSH,8);
    if(__gx->vNum!=8||!(__gx->dirtyState&(1u<<3))) return 1;
    unsigned char stateBytes[512];
    aurora::gx::fifo::begin_display_list(stateBytes,sizeof(stateBytes));
    GXFlush();
    aurora::gx::fifo::end_display_list();
    GXSetMisc(GX_MT_XF_FLUSH,0);
    using namespace aurora::gx::fifo;
    // A native GX object followed by a retail J3D display-list binding must
    // resolve the new MEM1 image and discard the old object's cache identity.
    alignas(32) static unsigned char textureMemory[0x4000]{};
    aurora::g_config.mem1Size=sizeof(textureMemory);
    MEM1Start=textureMemory; MEM1End=textureMemory+sizeof(textureMemory);
    OSBaseAddress=reinterpret_cast<uintptr_t>(textureMemory);
    auto& texture=aurora::gx::g_gxState.loadedTextures[0];
    texture.data=reinterpret_cast<void*>(0x12345678);
    texture.texObjId=123;
    texture.texDataVersion=9;
    texture.mWidth=7; texture.mHeight=9; texture.mFormat=GX_TF_RGBA8;
    handle_bp(0x94000100); // physical 0x2000
    handle_bp(0x8860fc7f); // RGBA8, 128 x 64
    handle_bp(0x800000c0); // trilinear mip filtering
    handle_bp(0x84004000); // maximum LOD 4
    if(texture.data!=OSPhysicalToCached(0x2000) || texture.texObjId!=0 ||
       texture.width()!=128 || texture.height()!=64 || texture.format()!=GX_TF_RGBA8 ||
       texture.mip_count()!=5) {
        std::fputs("Retail BP texture binding retained stale native metadata\n",stderr); return 1;
    }
    handle_bp(0x886fffff); // RGBA8, maximum 1024 x 1024
    if(texture.width()!=1024 || texture.height()!=1024) return 1;
    handle_bp(0x94000000);
    if(texture.data!=nullptr) return 1;
    handle_bp(0x80000080); // linear, no mipmaps
    if(texture.mip_count()!=1) return 1;
    init();
    if(GXSetDrawSyncCallback(recordToken)!=nullptr) return 1;
    begin_display_list(bytes,sizeof(bytes));
    GXSetDrawSync(0xffff);
    GXSetDrawSync(0);
    const auto tokenLength=end_display_list();
    const unsigned char tokenBytes[]={0x61,0x48,0,0xff,0xff,0x61,0x47,0,0xff,0xff,
                                     0x61,0x48,0,0,0,0x61,0x47,0,0,0};
    if(tokenLength!=32||std::memcmp(bytes,tokenBytes,sizeof(tokenBytes))||tokenCount.load()!=0) {
        std::fputs("Draw-sync recording must preserve tokens without invoking callbacks\n",stderr);return 1;
    }
    begin_frame();
    for(unsigned i=0;i<128;++i) GXSetDrawSync(static_cast<u16>(i));
    GXCallDisplayList(bytes,tokenLength);
    drain();
    if(tokenCount.load()!=130||tokens[128]!=0xffff||tokens[129]!=0||callbackThread==std::this_thread::get_id()) {
        std::fputs("FIFO draw-sync dispatch lost ordering, token bits, or worker ownership\n",stderr);return 1;
    }
    for(unsigned i=0;i<128;++i) if(tokens[i]!=i) return 1;
    if(GXSetDrawSyncCallback(nullptr)!=recordToken) return 1;
    GXSetDrawSync(42);drain();
    if(tokenCount.load()!=130) return 1;
    end_frame();shutdown();
    std::puts("FIFO command bytes, display-list replay and 130 ordered worker draw-sync callbacks pass; no GPU completion is asserted.");
}
