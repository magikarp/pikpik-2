#include "p2_skin.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DFileBlock.h"
#include <cstring>
#include <cmath>

namespace {
template<class T> T read(const unsigned char* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
}
P2EnvelopeData* p2_load_envelope(const J3DEnvelopeBlock* block, size_t available) {
    if(!block || available<sizeof(*block) || block->mBlockType!=J3DFBT_Envelope) return nullptr;
    const size_t size=block->mSize;
    if(size<sizeof(*block) || size>available) return nullptr;
    const auto* bytes=reinterpret_cast<const unsigned char*>(block);
    const size_t count=block->mCount;
    const size_t counts=block->mJointCountTableOffset, indices=block->mIndexTableOffset,
                 weights=block->mWeightTableOffset, matrices=block->mInvBindTableOffset;
    auto range=[&](size_t at,size_t n,size_t width) { return (!n && !at) || (at>=sizeof(*block) && at<=size && n<=(size-at)/width); };
    if(!range(counts,count,1)) return nullptr;
    size_t influences=0;
    for(size_t i=0;i<count;++i) influences+=bytes[counts+i];
    if(!range(indices,influences,2) || !range(weights,influences,4) || (matrices && !range(matrices,1,48))) return nullptr;
    const size_t matrixCount=matrices ? (size-matrices)/48 : 0;
    for(size_t i=0;i<influences;++i) {
        if(read<u16>(bytes+indices+i*2)>=matrixCount || !std::isfinite(read<float>(bytes+weights+i*4))) return nullptr;
    }
    const size_t countStart=sizeof(P2EnvelopeData);
    const size_t indexStart=(countStart+count+1)&~size_t(1);
    const size_t weightStart=(indexStart+influences*2+3)&~size_t(3);
    const size_t matrixStart=(weightStart+influences*4+31)&~size_t(31);
    auto* allocation=static_cast<unsigned char*>(p2_game_alloc(matrixStart+matrixCount*48,32,nullptr));
    if(!allocation) return nullptr;
    auto* result=reinterpret_cast<P2EnvelopeData*>(allocation);
    result->count=count; result->influences=influences; result->matrices=matrixCount;
    result->counts=count ? allocation+countStart : nullptr;
    result->indices=influences ? reinterpret_cast<u16*>(allocation+indexStart) : nullptr;
    result->weights=influences ? reinterpret_cast<float*>(allocation+weightStart) : nullptr;
    result->inverseBind=matrixCount ? reinterpret_cast<float(*)[3][4]>(allocation+matrixStart) : nullptr;
    if(count) std::memcpy(result->counts,bytes+counts,count);
    for(size_t i=0;i<influences;++i) {
        result->indices[i]=read<u16>(bytes+indices+i*2);
        result->weights[i]=read<float>(bytes+weights+i*4);
    }
    for(size_t i=0;i<matrixCount;++i) for(size_t r=0;r<3;++r) for(size_t c=0;c<4;++c)
        result->inverseBind[i][r][c]=read<float>(bytes+matrices+i*48+r*16+c*4);
    return result;
}
