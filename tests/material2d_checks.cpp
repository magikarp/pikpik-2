#include "p2_assets.h"
#include "p2_material.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include "JSystem/J2D/J2DMaterialFactory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool ok,const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
template<class T> static T read(const u8* p) { return p2_read_big<T>(p); }
int main(int argc,char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"disc and arena");
    auto* heap=JKRExpHeap::createRoot(16,false); require(heap,"root heap");
    const auto archive=p2::decompressYaz0(p2::readAsset(argv[1],"new_screen/eng/title.szs"));
    size_t blocks=0, materials=0;
    for(const auto& entry:p2::readRarc(archive)) {
        const auto data=p2::decompressYaz0(p2::Bytes(archive.begin()+entry.offset,archive.begin()+entry.offset+entry.size));
        if(data.size()<32 || std::memcmp(data.data(),"SCRN",4)) continue;
        size_t cursor=32;
        for(u32 i=0;i<read<u32>(data.data()+12);++i) {
            require(cursor+8<=data.size(),"layout block header");
            const auto* b=data.data()+cursor;
            const size_t size=read<u32>(b+4); require(size>=8 && size<=data.size()-cursor,"layout block extent");
            cursor+=size;
            if(std::memcmp(b,"MAT1",4)) continue;
            auto* disk=reinterpret_cast<const J2DMaterialBlock*>(b);
            const u32 before=heap->getTotalFreeSize();
            void* decoded=p2_decode_2d_material(disk,size);
            if(!decoded) { std::fprintf(stderr,"Rejected MAT1 %s, size %zu count %u\n",entry.path.c_str(),size,read<u16>(b+8)); return 1; }
            p2_game_free(decoded);
            {
                J2DMaterialFactory factory(*disk);
                require(factory._00==read<u16>(b+8),"native material count");
                const size_t initAt=read<u32>(b+12), remapAt=read<u32>(b+16);
                for(u16 n=0;n<factory._00;++n) {
                    const u16 index=read<u16>(b+remapAt+n*2);
                    require(factory.mMatIndexTable[n]==index,"native remap index");
                    const auto* init=b+initAt+index*0xe8;
                    const auto* native=reinterpret_cast<const u8*>(factory.mMaterialInitData+index);
                    for(size_t at=0;at<0xe8;) {
                        if((at>=8 && at<0x52) || at>=0x72) {
                            u16 v; std::memcpy(&v,native+at,2); require(v==read<u16>(init+at),"native material initializer words"); at+=2;
                        } else { require(native[at]==init[at],"native material initializer bytes"); ++at; }
                    }
                    require(factory.getMaterialMode(n)==init[0] && factory.getMaterialAlphaCalc(n)==init[6],"original factory mode and alpha");
                    require(factory.countStages(n)<=16,"original stage count");
                    if(init[1]!=0xff) require(factory.newCullMode(n)==read<u32>(b+read<u32>(b+0x1c)+init[1]*4),"native culling enum");
                    for(int slot=0;slot<10;++slot) {
                        const u16 ref=read<u16>(init+0x24+slot*2);
                        if(ref==0xffff) continue;
                        const auto* original=b+read<u32>(b+0x34)+ref*36;
                        const auto* matrix=reinterpret_cast<const u8*>(factory.mTexMtxInfo+ref);
                        for(size_t at=4;at<36;at+=4) {
                            float v; std::memcpy(&v,matrix+at,4);
                            require(v==read<float>(original+at),"native texture pivot and SRT floats");
                        }
                    }
                    for(int slot=0;slot<4;++slot) {
                        const u16 ref=read<u16>(init+0x92+slot*2);
                        if(ref==0xffff) continue;
                        const auto* original=b+read<u32>(b+0x44)+ref*8;
                        const auto* color=reinterpret_cast<const u8*>(factory.mTevColor+ref);
                        for(size_t at=0;at<8;at+=2) {
                            s16 v; std::memcpy(&v,color+at,2); require(v==read<s16>(original+at),"native signed TEV colors");
                        }
                    }
                    for(int slot=0;slot<8;++slot) {
                        const u16 ref=read<u16>(init+0x38+slot*2);
                        require(factory.newTexNo(n,slot)==(ref==0xffff ? 0xffff:read<u16>(b+read<u32>(b+0x38)+ref*2)),"original texture-reference lookup");
                    }
                    const u16 font=read<u16>(init+0x48);
                    require(factory.newFontNo(n)==(font==0xffff ? 0xffff:read<u16>(b+read<u32>(b+0x3c)+font*2)),"original font-reference lookup");
                    for(int slot=0;slot<2;++slot) {
                        const u16 ref=read<u16>(init+8+slot*2);
                        auto color=factory.newMatColor(n,slot);
                        if(ref!=0xffff) require(!std::memcmp(&color,b+read<u32>(b+0x20)+ref*4,4),"original material RGBA");
                    }
                    ++materials;
                }
            }
            require(heap->check() && heap->getTotalFreeSize()==before,"factory releases native copy");
            require(!p2_decode_2d_material(disk,size-1),"truncated MAT1 rejected");
            auto damaged=p2::Bytes(b,b+size);
            const size_t remap=read<u32>(b+16);
            if(read<u16>(b+8)) {
                damaged[remap]=0xff; damaged[remap+1]=0xfe;
                require(!p2_decode_2d_material(reinterpret_cast<const J2DMaterialBlock*>(damaged.data()),size),"invalid material remap rejected");
            }
            ++blocks;
        }
    }
    require(blocks==7 && materials>0,"all title MAT1 blocks covered");
    std::printf("Original J2D material factory: %zu title blocks, %zu materials.\n",blocks,materials);
}
