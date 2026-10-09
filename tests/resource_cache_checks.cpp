#include "ARAM.h"
#include "Pikmin2ARAM.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
static void require(bool ok,const char* message) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"arena");
    auto* root=JKRExpHeap::createRoot(16,false);
    require(root && p2_dvd_mount(argv[1]),"heap and disc");
    auto* aram=JKRAram::create(0x800000,0x600000,16,16,16);
    require(aram,"ARAM manager");
    auto packed=p2::readAsset(std::filesystem::path(argv[1])/"files","user/Ebisawa/title/title.szs");
    auto title=p2::decompressResource(packed);
    auto list=p2::readAsset(std::filesystem::path(argv[1])/"files","enemy/enemyResList.txt");
    const auto initial=aram->getAramHeap()->getFreeSize();
    u32 baseline=0;
    for(int round=0;round<3;++round) {
      {
        ARAM::Mgr cache;
        Pikmin2ARAM::Mgr residents;
        residents.loadEnemy(); residents.loadDemo(); residents.loadItem();
        std::istringstream input(std::string(list.begin(),list.end()));
        std::string name; int enemies=0;
        while(input>>name && name!="EOF") { require(cache.search(name.c_str()),"enemy registered from supplied list"); ++enemies; }
        require(cache.mResourceList.getChildCount()==enemies+43,"original enemy, movie and item registrations");
        require(aram->getAramHeap()->getFreeSize()==initial,"registration is lazy");
        char path[]="/user/Ebisawa/title/title.szs";
        cache.dvdToAram(path,true); path[1]='X';
        const char* key="/user/Ebisawa/title/title.szs";
        auto* node=cache.search(key); require(node && !node->mMemoryBlock,"cache owns path and defers load");
        cache.dvdToAram(key,true);
        require(cache.mResourceList.getChildCount()==enemies+44,"duplicate registration reuses node");
        for(auto direction:{JKRDvdRipper::ALLOC_DIR_TOP,JKRDvdRipper::ALLOC_DIR_BOTTOM}) {
            u32 written=99;
            auto* bytes=cache.aramToMainRam(key,nullptr,0,0,Switch_1,0,root,direction,3,&written);
            require(bytes && written==title.size() && !std::memcmp(bytes,title.data(),written),"lazy title expansion in both allocation directions");
            root->free(bytes);
            u8 borrowed[80]; std::memset(borrowed,0x5a,sizeof(borrowed));
            require(cache.aramToMainRam(key,borrowed,0,0,Switch_1,64,root,direction,-1,&written)==borrowed &&
                written==64 && !std::memcmp(borrowed,title.data(),64) && borrowed[64]==0x5a,"borrowed buffer preserved including tail direction");
        }
        auto worker=[&] {
            for(int i=0;i<20;++i) {
                u8 bytes[64]; u32 written;
                require(cache.aramToMainRam(key,bytes,0,0,Switch_1,64,root,JKRDvdRipper::ALLOC_DIR_TOP,-1,&written)==bytes &&
                    written==64 && !std::memcmp(bytes,title.data(),64),"concurrent cached reads");
            }
        };
        std::thread a(worker),b(worker); a.join(); b.join();
        auto* tiny=JKRExpHeap::create(4096,root,false);
        require(tiny,"small destination heap");
        // Exercise decoding while the small destination is also current.
        tiny->becomeCurrentHeap(); root->becomeSystemHeap();
        const auto freeBefore=tiny->getTotalFreeSize();
        const u32 cap=tiny->getMaxAllocatableSize(32)*3/4;
        require(cap<title.size(),"failure check uses a bounded title prefix");
        u32 failedCount=99;
        require(!cache.aramToMainRam(key,nullptr,0,0,Switch_1,cap,tiny,JKRDvdRipper::ALLOC_DIR_BOTTOM,-1,&failedCount) &&
            !failedCount && tiny->getTotalFreeSize()==freeBefore,"tail allocation failure releases temporary buffer and clears size");
        require(JKRHeap::sCurrentHeap==tiny,"loader preserves the caller's current heap");
        const char* coldKey="user/Ebisawa/title/title.szs";
        cache.dvdToAram(coldKey,true);
        u8 cold[64];
        require(cache.aramToMainRam(coldKey,cold,0,0,Switch_1,64,tiny,JKRDvdRipper::ALLOC_DIR_BOTTOM,-1,&failedCount)==cold &&
            failedCount==64 && !std::memcmp(cold,title.data(),64),"cold DVD and ARAM decoding do not consume the tiny current heap");
        require(tiny->getTotalFreeSize()==freeBefore,"temporary loader storage leaves no game-heap allocations");
        root->becomeCurrentHeap();
        tiny->destroy();
        u32 written=99;
        require(!cache.aramToMainRam("missing",nullptr,0,0,Switch_1,0,root,JKRDvdRipper::ALLOC_DIR_BOTTOM,-1,&written) && !written,"unknown entry fails cleanly");
        require(!cache.dvdToAram("missing",false) && !cache.search("missing"),"failed eager load leaves no node");
        cache.dvdToAram("missing",true); written=99;
        require(!cache.aramToMainRam("missing",nullptr,0,0,Switch_1,0,root,JKRDvdRipper::ALLOC_DIR_BOTTOM,-1,&written) && !written,"failed lazy load leaves output empty");
      }
      require(!gAramMgr && aram->getAramHeap()->getFreeSize()==initial,"cache destruction releases names, nodes and ARAM");
      if(!round) baseline=root->getTotalFreeSize();
      else require(root->getTotalFreeSize()==baseline,"repeated cache lifetimes balance game heap");
    }
    delete aram;
    std::puts("Resource cache: resident registration, lazy title loads, borrowed buffers, concurrent reads and teardown pass.");
}
