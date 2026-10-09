#include "JSystem/J2D/J2DScreen.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_memory.h"
#include "p2_assets.h"
#include "p2_animation.h"
#include "JSystem/J2D/J2DAnmLoader.h"
#include "JSystem/J2D/J2DAnm.h"
#include <cstring>
#include <filesystem>
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main(int argc, const char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    require(argc==2 && p2_memory_init(128*1024*1024),"native memory");
    auto* root=JKRExpHeap::createRoot(16,false);
    require(root && p2_dvd_mount(argv[1]),"heap and disc");
    auto* arc=JKRArchive::mount("/new_screen/eng/title.szs",JKRArchive::EMM_Mem,root,JKRArchive::EMD_Head);
    require(arc,"title archive");
    {
        J2DScreen screen;
        require(screen.set("title_menu_6.blo",0x1100000,arc),"original title layout constructor");
        require(screen.search(0x4e67616d65ULL) && screen.search(0x4e6f7074696f6eULL) && screen.search(0x4e6f6d616b65ULL),"title menu hierarchy and tags");
        require(root->check(),"heap remains valid after full layout construction");
        std::printf("Original title menu layout constructed; %u bytes free.\n",root->getTotalFreeSize());
    }
    const char* others[]={"push_start_a.blo","push_start_b.blo","push_start_c.blo","push_start_d.blo","tm_2003nintendo.blo","tm_back.blo"};
    for (auto name:others) {
        J2DScreen screen;
        require(screen.set(name,0x1100000,arc),"other title layout constructor");
        require(root->check(),"layout heap integrity");
        std::printf("Constructed %s\n",name);
    }
    arc->unmount();
    const auto data=p2::decompressYaz0(p2::readAsset(std::filesystem::path(argv[1])/"files","new_screen/eng/omake.szs"));
    unsigned patterns=0;
    for(const auto& entry:p2::readRarc(data)) {
        const auto* b=data.data()+entry.offset;
        if(entry.size>=64 && !std::memcmp(b,"J3D1bpk1",8)) {
            J2DAnmColorKey color;
            require(p2_load_2d_color(&color,b+32,entry.size-32),"bonus-screen color animation");
        }
        if(entry.size<64 || std::memcmp(b,"J3D1btp1",8)) continue;
        const auto* block=b+32;
        const u32 before=root->getTotalFreeSize();
        auto* base=J2DAnmLoaderDataBase::load(b);
        require(base && base->getKind()==J2DANM_TexturePattern,"original pattern loader");
        auto* animation=static_cast<J2DAnmTexPattern*>(base);
        const unsigned table=p2_read_big<u32>(block+16), values=p2_read_big<u32>(block+20);
        for(unsigned i=0;i<animation->mUpdateMaterialNum;++i) {
            const unsigned count=p2_read_big<u16>(block+table+i*8), first=p2_read_big<u16>(block+table+i*8+2);
            require(animation->mNameTab.getName(i),"pattern material name");
            for(int frame=-1;frame<=int(count);++frame) {
                animation->setFrame(frame); u16 actual=0; animation->getTexNo(i,&actual);
                const unsigned index=frame<0 ? 0:frame>=int(count) ? count-1:frame;
                require(actual==p2_read_big<u16>(block+values+(first+index)*2),"pattern samples match disc including end clamps");
            }
        }
        auto* storage=animation->mNativeStorage;
        require(!p2_load_2d_pattern(animation,block,31) && animation->mNativeStorage==storage,"bad pattern preserves live storage");
        delete animation;
        require(root->check() && root->getTotalFreeSize()==before,"pattern teardown restores heap");
        ++patterns;
    }
    require(patterns>0,"real bonus-screen texture patterns exercised");
    std::printf("Original texture-pattern loader: %u animations passed.\n",patterns);
    require(root->check(),"heap remains valid after layout destruction");
}
