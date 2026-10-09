#include "p2_material.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DMaterialFactory.h"
#include <cstring>
#include <initializer_list>

namespace {
template<class T> T read(const u8* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
}
void* p2_decode_material(const J3DMaterialBlock* block,size_t available) {
    static_assert(sizeof(J3DMaterialInitData)==0x14c,"MAT3 initializer width");
    static_assert(sizeof(J3DIndInitData)==0x138,"MAT3 indirect initializer width");
    static_assert(sizeof(J3DTexMtxInfo)==100,"MAT3 texture matrix width");
    static_assert(sizeof(J3DFogInfo)==44,"MAT3 fog width");
    static_assert(sizeof(J3DNBTScaleInfo)==16,"MAT3 NBT scale width");
    if(!block || available<sizeof(*block) || block->mBlockType!=J3DFBT_Material) return nullptr;
    const size_t size=block->mSize, count=block->mNumMaterials;
    if(size<sizeof(*block) || size>available) return nullptr;
    const auto* source=reinterpret_cast<const u8*>(block);
    u32 offsets[30]={}; size_t spans[30]={};
    for(unsigned i=0;i<30;++i) {
        offsets[i]=read<u32>(source+12+4*i);
        if(offsets[i] && (offsets[i]<sizeof(*block) || offsets[i]>size)) return nullptr;
    }
    for(unsigned i=0;i<30;++i) {
        if(!offsets[i]) continue;
        size_t end=size;
        for(unsigned j=0;j<30;++j) if(offsets[j]>offsets[i] && offsets[j]<end) end=offsets[j];
        spans[i]=end-offsets[i];
    }
    const bool indirect=offsets[3] && u32(offsets[3]-offsets[2])>4;
    if(count>spans[1]/2 || (count && !offsets[0]) || (indirect && count>spans[3]/0x138)) return nullptr;
    size_t used[30]={}; used[1]=count;
    auto valid=[&](unsigned table,u32 index,unsigned width,u32 missing) {
        if(index==missing) return true;
        if(!offsets[table] || index>=spans[table]/width) return false;
        if(used[table]<size_t(index)+1) used[table]=size_t(index)+1;
        return true;
    };
    struct IndexGroup { unsigned start,count,table,width; };
    const IndexGroup groups[]={
        {0x08,2,5,4},{0x0c,4,7,8},{0x14,2,8,4},
        {0x28,8,11,4},{0x48,10,13,100},
        {0x84,8,15,2},{0x94,4,18,4},{0xbc,16,16,4},{0xdc,4,17,8},
        {0xe4,16,20,20},{0x104,16,21,4},{0x124,16,22,4},
        {0x144,1,23,44},{0x146,1,24,8},{0x148,1,25,4},{0x14a,1,29,16}};
    const unsigned byteTables[]={4,6,10,19,27,26,28};
    const unsigned byteWidths[]={4,1,1,1,1,4,1};
    for(size_t n=0;n<count;++n) {
        const u16 index=read<u16>(source+offsets[1]+2*n);
        if(index>=spans[0]/0x14c) return nullptr;
        if(used[0]<size_t(index)+1) used[0]=size_t(index)+1;
        const auto* init=source+offsets[0]+index*0x14c;
        for(unsigned i=0;i<7;++i) if(!valid(byteTables[i],init[1+i],byteWidths[i],255)) return nullptr;
        for(const auto& group:groups) for(unsigned j=0;j<group.count;++j)
            if(!valid(group.table,read<u16>(init+group.start+2*j),group.width,65535)) return nullptr;
        if(init[2]!=255 && source[offsets[6]+init[2]]>2) return nullptr;
        if(init[3]!=255 && source[offsets[10]+init[3]]>8) return nullptr;
        if(init[4]!=255 && source[offsets[19]+init[4]]>16) return nullptr;
        if(indirect) {
            const auto* ind=source+offsets[3]+n*0x138;
            if(ind[0]>1 || (ind[0] && ind[1]>3)) return nullptr;
        }
    }
    const unsigned alignments[30]={2,2,1,4,4,1,1,1,1,1,1,1,1,4,4,2,1,2,1,1,1,1,1,4,1,1,1,1,1,4};
    for(unsigned i=0;i<30;++i)
        if((used[i] || (i==3 && indirect)) && offsets[i]%alignments[i]) return nullptr;
    // Light/post-transform slots are not consumed by this factory. Retail data
    // can leave zero indices in these slots even when their tables are absent.
    // An absent table may share its offset with its successor in retail data.
    // Only tables actually consumed by the original factory are converted.
    auto* native=static_cast<u8*>(p2_game_alloc(size,32,nullptr));
    if(!native) return nullptr;
    std::memcpy(native,source,size);
    auto convert16=[&](size_t at) { const u16 v=read<u16>(source+at); std::memcpy(native+at,&v,2); };
    auto convert32=[&](size_t at) { const u32 v=read<u32>(source+at); std::memcpy(native+at,&v,4); };
    for(size_t n=0;n<used[0];++n) {
        const size_t base=offsets[0]+n*0x14c;
        for(unsigned at=8;at<0x9c;at+=2) convert16(base+at);
        for(unsigned at=0xbc;at<0x14c;at+=2) convert16(base+at);
    }
    for(unsigned table:{1u,15u,17u}) for(size_t at=0;at<used[table]*(table==17 ? 8 : 2);at+=2) convert16(offsets[table]+at);
    for(size_t n=0;n<used[4];++n) convert32(offsets[4]+4*n);
    for(unsigned table:{13u,14u}) for(size_t n=0;n<used[table];++n) {
        const size_t base=offsets[table]+100*n;
        for(unsigned at=4;at<24;at+=4) convert32(base+at);
        convert16(base+24); // SRT rotation; bytes 26-27 are padding.
        for(unsigned at=28;at<100;at+=4) convert32(base+at);
    }
    for(size_t n=0;n<used[23];++n) {
        const size_t base=offsets[23]+44*n;
        convert16(base+2);
        for(unsigned at=4;at<20;at+=4) convert32(base+at);
        for(unsigned at=24;at<44;at+=2) convert16(base+at);
    }
    for(size_t n=0;n<used[29];++n) for(unsigned at=4;at<16;at+=4) convert32(offsets[29]+16*n+at);
    if(indirect) for(size_t n=0;n<count;++n) for(unsigned m=0;m<3;++m)
        for(unsigned f=0;f<6;++f) convert32(offsets[3]+n*0x138+0x14+m*28+f*4);
    return native;
}
