#include "p2_particle.h"
#include "p2_endian.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>

namespace {
using u8=std::uint8_t;
using u16=std::uint16_t;
using u32=std::uint32_t;
u16 word(const u8* p) { return p2_read_big<u16>(p); }
u32 dword(const u8* p) { return p2_read_big<u32>(p); }
bool range(std::size_t size,std::size_t offset,std::size_t count) { return offset<=size && count<=size-offset; }
bool tag(const u8* p,const char* name) { return !std::memcmp(p,name,4); }
bool texture(const u8* p,std::size_t size) {
    // TEX1 has a 32-byte name/header, followed by a ResTIMG and its payload.
    if(size<64 || !std::memchr(p+12,0,20)) return false;
    p+=32; size-=32;
    unsigned w=word(p+2),h=word(p+4),bw=4,bh=4,block=32;
    if(!w || !h) return false;
    switch(p[0]) {
    case 0: case 8: case 14: bw=8; bh=8; break;
    case 1: case 2: case 9: bw=8; break;
    case 3: case 4: case 5: case 10: break;
    case 6: block=64; break;
    default: return false;
    }
    unsigned levels=p[16] ? p[24] : 1;
    if(!levels || levels>16) return false;
    std::size_t bytes=0;
    for(unsigned i=0;i<levels;++i) {
        bytes+=std::size_t((w+bw-1)/bw)*((h+bh-1)/bh)*block;
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    const auto image=dword(p+28) ? dword(p+28) : 32;
    if(image<32 || !range(size,image,bytes)) return false;
    if(word(p+10) && (dword(p+12)<32 || !range(size,dword(p+12),2*word(p+10)))) return false;
    return true;
}
}
bool p2_validate_jpc(const void* data,std::size_t size,const char** error) {
    if(error) *error=nullptr;
    auto fail=[&](const char* why) { if(error) *error=why; return false; };
    if(!data || size<16) return fail("truncated JPAC header");
    const auto* bytes=static_cast<const u8*>(data);
    if(std::memcmp(bytes,"JPAC2-10",8)) return fail("unsupported JPAC version");
    const unsigned resources=word(bytes+8),textures=word(bytes+10);
    const std::size_t textureStart=dword(bytes+12);
    if(textureStart<16 || textureStart>size || (textureStart&3)) return fail("invalid texture section offset");
    std::size_t at=16;
    for(unsigned r=0;r<resources;++r) {
        if(!range(textureStart,at,8)) return fail("truncated resource header");
        const auto* header=bytes+at;
        const unsigned count=word(header+2),fieldCount=header[4],keyCount=header[5],textureCount=header[6];
        unsigned fields=0,keys=0,dynamics=0,bases=0,ids=0; at+=8;
        for(unsigned b=0;b<count;++b) {
            if(!range(textureStart,at,8)) return fail("truncated particle block");
            const auto* p=bytes+at; const std::size_t length=dword(p+4);
            if(length<8 || (length&3) || !range(textureStart,at,length)) return fail("invalid particle block size");
            if(tag(p,"BEM1")) {
                if(length<0x7c || ++dynamics!=1 || ((dword(p+8)>>8)&7)>6) return fail("invalid emitter block");
            } else if(tag(p,"BSP1")) {
                if(length<0x34 || ++bases!=1) return fail("invalid base shape block");
                const unsigned blend=word(p+24);
                if((blend&3)>2 || ((blend>>2)&15)>9 || ((blend>>6)&15)>9 || ((dword(p+8)>>15)&7)>5)
                    return fail("invalid base shape lookup index");
                std::size_t table=0x34;
                if(dword(p+8)&0x01000000) table+=40;
                if(table>length) return fail("truncated texture-coordinate animation");
                if(p[30]&1) {
                    if(!p[31] || !range(length,table,p[31])) return fail("truncated texture animation");
                    for(unsigned i=0;i<p[31];++i) if(p[table+i]>=textureCount) return fail("texture animation index outside resource");
                }
                if(p[32]>=textureCount) return fail("base texture index outside resource");
                for(unsigned c=0;c<2;++c) if(p[33]&(c ? 8 : 2)) {
                    const unsigned offset=word(p+12+2*c),n=p[34+c];
                    if(!n || offset<0x34 || (offset&1) || !range(length,offset,6*n) || std::int16_t(word(p+36))<0)
                        return fail("invalid color animation extent");
                    for(unsigned k=0;k<n;++k) {
                        const auto frame=std::int16_t(word(p+offset+6*k));
                        if(frame<0 || (k && frame<=std::int16_t(word(p+offset+6*(k-1))))) return fail("unordered color keys");
                    }
                }
            } else if(tag(p,"FLD1")) {
                if(length<0x44 || ++fields>fieldCount || (dword(p+8)&15)>8) return fail("invalid field block");
            } else if(tag(p,"KFA1")) {
                if(length<12 || ++keys>keyCount || !p[9] || !range(length,12,16*p[9])) return fail("invalid keyframe block");
                float last=-std::numeric_limits<float>::infinity();
                for(unsigned k=0;k<p[9];++k) {
                    const float frame=p2_read_big<float>(p+12+16*k);
                    if(!std::isfinite(frame) || frame<=last) return fail("unordered particle keyframes");
                    for(unsigned v=1;v<4;++v) if(!std::isfinite(p2_read_big<float>(p+12+16*k+4*v))) return fail("nonfinite particle key value");
                    last=frame;
                }
                if(p[11] && (last<0 || last>=2147483520.0f)) return fail("invalid particle keyframe loop period");
            } else if(tag(p,"ESP1")) {
                if(length<0x60) return fail("truncated extra shape");
            } else if(tag(p,"SSP1")) {
                if(length<0x48) return fail("truncated child shape");
            } else if(tag(p,"ETX1")) {
                if(length<0x28) return fail("truncated indirect texture shape");
                if(((dword(p+8)&1) && p[37]>=textureCount) || ((dword(p+8)&0x100) && p[38]>=textureCount))
                    return fail("indirect texture index outside resource");
            } else if(tag(p,"TDB1")) {
                if(++ids!=1 || !range(length,8,2*textureCount)) return fail("invalid texture ID table");
                for(unsigned i=0;i<textureCount;++i) if(word(p+8+2*i)>=textures) return fail("texture ID outside archive");
            } else return fail("unknown particle block type");
            at+=length;
        }
        if(dynamics!=1 || bases!=1 || fields!=fieldCount || keys!=keyCount || (textureCount && ids!=1))
            return fail("particle resource block counts disagree");
    }
    at=textureStart;
    for(unsigned i=0;i<textures;++i) {
        if(!range(size,at,8)) return fail("truncated texture block");
        const auto* p=bytes+at; const std::size_t length=dword(p+4);
        if(!tag(p,"TEX1") || (length&3) || !range(size,at,length) || !texture(p,length)) return fail("invalid particle texture payload");
        at+=length;
    }
    return true;
}
