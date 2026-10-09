#include "p2_shape.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DShapeFactory.h"
#include <cstring>
#include <cmath>
#include <initializer_list>

namespace {
template<class T> T read(const u8* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
}
bool p2_load_shape(J3DShapeFactory* out,const J3DShapeBlock* block,size_t available) {
    static_assert(sizeof(J3DShapeInitData)==40,"shape initializer disk width");
    static_assert(sizeof(GXVtxDescList)==8,"vertex descriptor disk width");
    if(!out || !block || available<sizeof(*block) || block->mBlockType!=J3DFBT_Shape) return false;
    const size_t size=block->mSize, count=block->mShapeNum;
    if(size<sizeof(*block) || size>available) return false;
    const u32 at[]={block->mShapeDataOffset,block->mRemapTableOffset,block->mNameTableOffset,
                    block->mAttribTableOffset,block->mMatrixTableOffset,block->mPrimDataOffset,
                    block->mMatrixInitDataOffset,block->mMtxGroupTableOffset};
    size_t spans[8]={};
    for(unsigned i=0;i<8;++i) {
        if(!at[i]) continue;
        if(at[i]<sizeof(*block) || at[i]>=size || (at[i]&3)) return false;
        size_t end=size;
        for(unsigned j=0;j<8;++j) {
            if(i!=j && at[j]==at[i]) return false;
            if(at[j]>at[i] && at[j]<end) end=at[j];
        }
        spans[i]=end-at[i];
    }
    if(count && (!at[0] || count>spans[1]/2 || !at[3])) return false;
    const auto* disk=reinterpret_cast<const u8*>(block);
    // Validate all references before allocating or replacing the current data.
    for(size_t n=0;n<count;++n) {
        const u16 mapped=read<u16>(disk+at[1]+2*n);
        if(mapped>=spans[0]/40) return false;
        const auto* shape=disk+at[0]+40*mapped;
        if(shape[0]>J3DShapeMtx_Multi) return false;
        const u16 groups=read<u16>(shape+2), desc=read<u16>(shape+4),
                  firstMtx=read<u16>(shape+6), firstDraw=read<u16>(shape+8);
        if(size_t(firstMtx)+groups>spans[6]/8 || size_t(firstDraw)+groups>spans[7]/8 || desc%8) return false;
        bool terminated=false;
        for(size_t d=desc;d+8<=spans[3];d+=8) {
            const u32 attr=read<u32>(disk+at[3]+d), type=read<u32>(disk+at[3]+d+4);
            if(attr==GX_VA_NULL) { terminated=true; break; }
            if(attr>GX_VA_TEX7 || type>GX_INDEX16) return false;
        }
        if(!terminated) return false;
        for(unsigned f=12;f<40;f+=4) if(!std::isfinite(read<float>(shape+f))) return false;
        for(unsigned g=0;g<groups;++g) {
            const auto* mtx=disk+at[6]+8*(size_t(firstMtx)+g);
            const u16 used=read<u16>(mtx+2);
            const u32 first=read<u32>(mtx+4);
            if(shape[0]==J3DShapeMtx_Multi && (first>spans[4]/2 || used>spans[4]/2-first)) return false;
            const auto* draw=disk+at[7]+8*(size_t(firstDraw)+g);
            const u32 length=read<u32>(draw), start=read<u32>(draw+4);
            if(start>spans[5] || length>spans[5]-start) return false;
        }
    }
    auto* native=static_cast<u8*>(p2_game_alloc(size,32,nullptr));
    if(!native) return false;
    std::memcpy(native,disk,size);
    // Convert from immutable source bytes so aliased shape remaps stay correct.
    for(size_t n=0;n<spans[0]/40;++n) {
        auto* shape=reinterpret_cast<J3DShapeInitData*>(native+at[0]+40*n);
        const auto* source=disk+at[0]+40*n;
        shape->mMtxGroupNum=read<u16>(source+2); shape->mVtxDescListIndex=read<u16>(source+4);
        shape->mShapeMtxInitDataIndex=read<u16>(source+6); shape->mShapeDrawInitDataIndex=read<u16>(source+8);
        shape->mRadius=read<float>(source+12);
        shape->mMin.x=read<float>(source+16); shape->mMin.y=read<float>(source+20); shape->mMin.z=read<float>(source+24);
        shape->mMax.x=read<float>(source+28); shape->mMax.y=read<float>(source+32); shape->mMax.z=read<float>(source+36);
    }
    for(unsigned table : {1u,4u}) for(size_t n=0;n<spans[table]/2;++n) {
        const u16 value=read<u16>(disk+at[table]+2*n); std::memcpy(native+at[table]+2*n,&value,2);
    }
    for(unsigned table : {3u,7u}) for(size_t n=0;n<spans[table]/4;++n) {
        const u32 value=read<u32>(disk+at[table]+4*n); std::memcpy(native+at[table]+4*n,&value,4);
    }
    for(size_t n=0;n<spans[6]/8;++n) {
        auto* mtx=reinterpret_cast<J3DShapeMtxInitData*>(native+at[6]+8*n);
        const auto* source=disk+at[6]+8*n;
        mtx->mUseMtxIndex=read<u16>(source); mtx->mUseMtxCount=read<u16>(source+2); mtx->mFirstUseMtxIndex=read<u32>(source+4);
    }
    p2_game_free(out->mNativeStorage); out->mNativeStorage=native;
    out->mInitData=at[0] ? reinterpret_cast<J3DShapeInitData*>(native+at[0]) : nullptr;
    out->mInitDataIndices=at[1] ? reinterpret_cast<u16*>(native+at[1]) : nullptr;
    out->mVtxDescLists=at[3] ? reinterpret_cast<GXVtxDescList*>(native+at[3]) : nullptr;
    out->mMtxTable=at[4] ? reinterpret_cast<u16*>(native+at[4]) : nullptr;
    out->mDisplayListData=at[5] ? native+at[5] : nullptr;
    out->mMtxInitData=at[6] ? reinterpret_cast<J3DShapeMtxInitData*>(native+at[6]) : nullptr;
    out->mDrawInitData=at[7] ? reinterpret_cast<J3DShapeDrawInitData*>(native+at[7]) : nullptr;
    out->mVcdVatCmdBuffer=nullptr;
    return true;
}
