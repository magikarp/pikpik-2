#include "p2_material.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J2D/J2DMaterialFactory.h"
#include <cstring>
#include <initializer_list>

namespace {
template<class T> T read(const u8* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
}
void* p2_decode_2d_material(const J2DMaterialBlock* block,size_t available) {
    static_assert(sizeof(J2DMaterialInitData)==0xe8,"MAT1 initializer width");
    static_assert(sizeof(J2DIndInitData)==0x128,"MAT1 indirect initializer width");
    static_assert(sizeof(J2DTexMtxInfo)==36,"MAT1 texture matrix width");
    if(!block || available<sizeof(*block)) return nullptr;
    const auto* source=reinterpret_cast<const u8*>(block);
    const size_t size=read<u32>(source+4), count=block->_08;
    if(read<u32>(source)!=0x4d415431 || size<sizeof(*block) || size>available) return nullptr;
    u32 offsets[23]={}; size_t spans[23]={},used[23]={};
    for(unsigned i=0;i<23;++i) {
        offsets[i]=read<u32>(source+12+4*i);
        if(offsets[i] && (offsets[i]<sizeof(*block) || offsets[i]>size)) return nullptr;
    }
    for(unsigned i=0;i<23;++i) {
        if(!offsets[i]) continue;
        size_t end=size;
        for(unsigned j=0;j<23;++j) if(offsets[j]>offsets[i] && offsets[j]<end) end=offsets[j];
        spans[i]=end-offsets[i];
    }
    const bool indirect=offsets[3] && u32(offsets[3]-offsets[2])>4;
    if(count>spans[1]/2 || (count && !offsets[0]) || (indirect && count>spans[3]/0x128)) return nullptr;
    used[1]=count;
    auto valid=[&](unsigned table,u32 index,unsigned width,u32 missing) {
        if(index==missing) return true;
        if(!offsets[table] || index>=spans[table]/width) return false;
        if(used[table]<size_t(index)+1) used[table]=size_t(index)+1;
        return true;
    };
    struct IndexGroup { unsigned start,count,table,width; };
    const IndexGroup groups[]={
        {0x08,2,5,4},{0x0c,4,7,4},{0x14,8,9,4},{0x24,10,10,36},
        {0x38,8,11,2},{0x48,1,12,2},{0x4a,4,15,4},{0x72,16,13,4},
        {0x92,4,14,8},{0x9a,16,17,20},{0xba,16,18,4},{0xda,4,19,4},
        {0xe2,1,20,8},{0xe4,1,21,4}};
    const unsigned byteTables[]={4,6,8,16,22}, byteWidths[]={4,1,1,1,1};
    for(size_t n=0;n<count;++n) {
        const u16 index=read<u16>(source+offsets[1]+2*n);
        if(index>=spans[0]/0xe8) return nullptr;
        if(used[0]<size_t(index)+1) used[0]=size_t(index)+1;
        const auto* init=source+offsets[0]+index*0xe8;
        for(unsigned i=0;i<5;++i) if(!valid(byteTables[i],init[1+i],byteWidths[i],255)) return nullptr;
        for(const auto& group:groups) for(unsigned j=0;j<group.count;++j)
            if(!valid(group.table,read<u16>(init+group.start+2*j),group.width,65535)) return nullptr;
        if(init[2]!=255 && source[offsets[6]+init[2]]>2) return nullptr;
        if(init[3]!=255 && source[offsets[8]+init[3]]>8) return nullptr;
        if(init[4]!=255 && source[offsets[16]+init[4]]>16) return nullptr;
        if(indirect) {
            const auto* ind=source+offsets[3]+n*0x128;
            if(ind[0]>1 || (ind[0] && ind[1]>4)) return nullptr;
        }
    }
    const unsigned alignments[]={2,2,1,4,4,1,1,1,1,1,4,2,2,1,2,1,1,1,1,1,1,1,1};
    for(unsigned i=0;i<23;++i) if((used[i] || (i==3 && indirect)) && offsets[i]%alignments[i]) return nullptr;
    auto* native=static_cast<u8*>(p2_game_alloc(size,32,nullptr));
    if(!native) return nullptr;
    std::memcpy(native,source,size);
    auto convert16=[&](size_t at) { const u16 v=read<u16>(source+at); std::memcpy(native+at,&v,2); };
    auto convert32=[&](size_t at) { const u32 v=read<u32>(source+at); std::memcpy(native+at,&v,4); };
    for(size_t n=0;n<used[0];++n) {
        const size_t base=offsets[0]+n*0xe8;
        for(unsigned at=8;at<0x52;at+=2) convert16(base+at);
        for(unsigned at=0x72;at<0xe8;at+=2) convert16(base+at);
    }
    for(unsigned table:{1u,11u,12u,14u}) for(size_t at=0;at<used[table]*(table==14 ? 8:2);at+=2) convert16(offsets[table]+at);
    for(size_t n=0;n<used[4];++n) convert32(offsets[4]+4*n);
    for(size_t n=0;n<used[10];++n) for(unsigned at=4;at<36;at+=4) convert32(offsets[10]+36*n+at);
    if(indirect) for(size_t n=0;n<count;++n) for(unsigned m=0;m<3;++m)
        for(unsigned f=0;f<6;++f) convert32(offsets[3]+n*0x128+0x0c+m*28+f*4);
    return native;
}
