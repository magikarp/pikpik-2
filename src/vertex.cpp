#include "p2_vertex.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DFileBlock.h"
#include "JSystem/J3D/J3DVertexData.h"
#include <cstring>

namespace {
u32 word(const unsigned char* b) { u32 v; std::memcpy(&v,b,4); return p2_big_endian(v); }
unsigned scalarWidth(u32 type) { return type<=1 ? 1 : type<=3 ? 2 : type==4 ? 4 : 0; }
unsigned components(unsigned slot,u32 count) {
    if (slot==0) return count<=1 ? count+2 : 0;
    if (slot==1 || slot==12) return count<=2 ? (count==0 ? 3 : 9) : 0;
    return count<=1 ? count+1 : 0;
}
}
bool p2_load_vertex(J3DVertexData* out,const J3DVertexBlock* block,size_t available) {
    if (!out || !block || available<sizeof(*block) || block->mBlockType!=J3DFBT_Vertex) return false;
    const size_t size=block->mSize;
    if (size<sizeof(*block) || size>available || size>UINT32_MAX-32) return false;
    const auto* source=reinterpret_cast<const unsigned char*>(block);
    const u32 format=block->mVertexFormatOffset;
    if (format<sizeof(*block) || format>size-16) return false;
    u32 offsets[13]={block->mPositionDataOffset,block->mNormalDataOffset,block->mColorDataOffset[0],block->mColorDataOffset[1]};
    for(int i=0;i<8;++i) offsets[4+i]=block->mTexCoordDataOffset[i];
    offsets[12]=block->mNBTDataOffset;
    GXVtxAttrFmtList formats[32] = {};
    size_t formatCount=0;
    bool terminated=false;
    for(;formatCount<32 && format+formatCount*16+16<=size;++formatCount) {
        const auto* p=source+format+formatCount*16;
        auto& f=formats[formatCount];
        f.mAttr=GXAttr(word(p)); f.mCount=GXCompCnt(word(p+4)); f.mType=GXCompType(word(p+8)); f.mFrac=p[12];
        if (f.mAttr==GX_VA_NULL) { ++formatCount; terminated=true; break; }
        if (!((f.mAttr>=GX_VA_POS && f.mAttr<=GX_VA_TEX7) || f.mAttr==GX_VA_NBT)) return false;
    }
    if (!terminated) return false;
    u32 lengths[13]={}, widths[13]={}, strides[13]={};
    for(unsigned slot=0;slot<13;++slot) {
        const u32 start=offsets[slot];
        if (!start) continue;
        if (start<sizeof(*block) || start>=size || (start>=format && start<format+formatCount*16)) return false;
        size_t end=size;
        if (format>start && format<end) end=format;
        for(auto next:offsets) if (next>start && next<end) end=next;
        lengths[slot]=end-start;
        const unsigned attr=slot==12 ? GX_VA_NBT : GX_VA_POS+slot;
        const GXVtxAttrFmtList* selected=nullptr;
        for(size_t i=0;i+1<formatCount;++i)
            if (formats[i].mAttr==attr || (slot==12 && formats[i].mAttr==GX_VA_NRM)) selected=&formats[i];
        if (!selected) return false;
        if (slot==2 || slot==3) {
            // Packed color bytes remain BE; RGBA8 is byte-order independent.
            const unsigned colorWidths[]={2,3,4,2,3,4};
            if (unsigned(selected->mType)>=6) return false;
            widths[slot]=1; strides[slot]=colorWidths[selected->mType];
        } else {
            widths[slot]=scalarWidth(selected->mType);
            strides[slot]=widths[slot]*components(slot,selected->mCount);
            if (!strides[slot] || start%widths[slot] || lengths[slot]%widths[slot]) return false;
        }
        if (slot==0 && out->mVtxNum>lengths[slot]/strides[slot]) return false;
        // Distinct attributes may not share a byte range with different formats.
        for(unsigned previous=0;previous<slot;++previous)
            if(offsets[previous]==start && (widths[previous]!=widths[slot] || strides[previous]!=strides[slot])) return false;
    }
    auto* native=static_cast<unsigned char*>(p2_game_alloc(size+32,32,nullptr));
    if(!native) return false;
    std::memcpy(native,source,size); std::memset(native+size,0,32);
    std::memcpy(native+format,formats,formatCount*sizeof(GXVtxAttrFmtList));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for(unsigned slot=0;slot<13;++slot) {
        bool duplicate=false;
        for(unsigned earlier=0;earlier<slot;++earlier) if(offsets[earlier]==offsets[slot]) duplicate=true;
        if(!offsets[slot] || duplicate || widths[slot]<=1) continue;
        for(u32 at=offsets[slot];at<offsets[slot]+lengths[slot];at+=widths[slot])
            for(unsigned i=0;i<widths[slot]/2;++i) {
                const u8 value=native[at+i]; native[at+i]=native[at+widths[slot]-1-i]; native[at+widths[slot]-1-i]=value;
            }
    }
#endif
    p2_game_free(out->mNativeStorage); out->mNativeStorage=native;
    auto pointer=[&](unsigned slot)->void* { return offsets[slot] ? native+offsets[slot] : nullptr; };
    out->mVtxAttrFmtList=reinterpret_cast<GXVtxAttrFmtList*>(native+format);
    out->mVtxPos=pointer(0); out->mVtxNorm=pointer(1); out->mVtxNBT=pointer(12);
    for(unsigned i=0;i<2;++i) out->mVtxColor[i]=static_cast<GXColor*>(pointer(2+i));
    for(unsigned i=0;i<8;++i) out->mVtxTexCoord[i]=pointer(4+i);
    for(unsigned i=0;i<13;++i) {
        out->mNativeArrayBytes[i]=lengths[i];
        out->mNativeArrayLittleEndian[i]=(i!=2 && i!=3);
    }
    out->mNormNum=strides[1] ? lengths[1]/strides[1] : 0;
    out->mColorNum=strides[2] ? lengths[2]/strides[2] : 0;
    out->mTexCoordNum=strides[4] ? lengths[4]/strides[4] : 0;
    for(size_t i=0;i+1<formatCount;++i) {
        if(formats[i].mAttr==GX_VA_POS) { out->mVtxPosType=formats[i].mType; out->mVtxPosFrac=formats[i].mFrac; }
        if(formats[i].mAttr==GX_VA_NRM || formats[i].mAttr==GX_VA_NBT) { out->mVtxNrmType=formats[i].mType; out->mVtxNrmFrac=formats[i].mFrac; }
    }
    return true;
}
