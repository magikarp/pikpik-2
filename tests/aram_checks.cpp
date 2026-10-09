#include "JSystem/JKernel/JKRAram.h"
#include "JSystem/JKernel/JKRDvdAramRipper.h"
#include "JSystem/JKernel/JKRDecomp.h"
#include "p2_assets.h"
#include "p2_aram.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
static uintptr_t callbackAddress;
static void completed(uintptr_t address) { callbackAddress=address; }
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"native arena and disc");
    auto* root=JKRExpHeap::createRoot(16,false); require(root && p2_dvd_mount(argv[1]),"root heap and DVD mount");
    auto compressed=p2::readAsset(std::filesystem::path(argv[1])/"files","user/Ebisawa/title/title.szs");
    auto expanded=p2::decompressResource(compressed);
    u32 baseline=0;
    for(unsigned round=0;round<5;++round) {
        auto* manager=JKRAram::create(0x800000,0x600000,16,16,16);
        require(manager && manager->getAudioMemory()==0x4000 && manager->getGraphMemory()==0x804000,
            "original boot reservations fit the native ARAM backing");
        auto* heap=manager->getAramHeap(); const auto free=heap->getFreeSize();
        require(free==0x600000 && !heap->alloc(0,JKRAramHeap::AM_Head) &&
            !heap->alloc(UINT32_MAX,JKRAramHeap::AM_Head),"allocator rejects zero and overflowing requests");
        auto* head=heap->alloc(33,JKRAramHeap::AM_Head);
        auto* tail=heap->alloc(65,JKRAramHeap::AM_Tail);
        require(head && tail && head->mSize==64 && tail->mSize==96 &&
            head->mAddress==manager->getGraphMemory() && tail->mAddress==manager->getGraphMemory()+0x600000-96,
            "original allocator places aligned head and tail blocks");
        auto* mainBytes=static_cast<u8*>(root->alloc(128,32));
        for(unsigned i=0;i<128;++i) mainBytes[i]=i^0xa5;
        ARQRequest request{}; callbackAddress=0;
        ARQPostRequest(&request,17,ARAM_DIR_MRAM_TO_ARAM,0,p2_host_to_physical(mainBytes),head->mAddress,64,completed);
        require(callbackAddress==reinterpret_cast<uintptr_t>(&request),"ARQ callback preserves a full-width stack pointer");
        std::memset(mainBytes,0,64);
        ARStartDMA(ARAM_DIR_ARAM_TO_MRAM,p2_host_to_physical(mainBytes),head->mAddress,64);
        for(unsigned i=0;i<64;++i) require(mainBytes[i]==(i^0xa5),"SDK transfer round trip uses MEM1 offsets");
        u8 raw[64]; u32 rawCount=0;
        require(JKRAram::aramToMainRam(head,raw,64,0,Switch_1,1,root,-1,&rawCount)==raw &&
            rawCount==64 && !std::memcmp(raw,mainBytes,64),"expansion limit does not truncate an uncompressed resource");
        require(!p2_aram_read(UINT32_MAX,mainBytes,1) && !p2_aram_write(ARGetSize()-1,mainBytes,2),"backing range checks prevent overflow");
        {
            JKRAMCommand command;
            command.mDirection=ARAM_DIR_MRAM_TO_ARAM; command.mSource=p2_host_to_physical(mainBytes);
            command.mDestination=tail->mAddress; command.mLength=64;
            auto* message=new JKRAramCommand; message->setting(ARAMMSG_DMA,&command);
            OSSendMessage(&JKRAram::sMessageQueue,message,OS_MESSAGE_BLOCK);
            OSMessage result; OSReceiveMessage(&command.mMessageQueue,&result,OS_MESSAGE_BLOCK);
            require(result==&command,"manager worker publishes completion for the actual command");
        }
        root->free(mainBytes); delete head; delete tail;
        require(heap->getFreeSize()==free,"original allocator coalesces released blocks");
        auto worker=[&] {
            for(unsigned i=0;i<250;++i) {
                auto* block=heap->alloc(32*(1+i%31),i%2 ? JKRAramHeap::AM_Head : JKRAramHeap::AM_Tail);
                require(block,"concurrent ARAM allocation"); delete block;
            }
        };
        std::thread a(worker),b(worker); a.join(); b.join();
        require(heap->getFreeSize()==free,"concurrent block deletion preserves the allocator list");
        u32 transferred=0;
        auto* resource=JKRAram::mainRamToAram(compressed.data(),0,compressed.size(),Switch_0,0,root,7,&transferred);
        require(resource && resource->mGroupID==7 && transferred==compressed.size(),"title archive cached in native ARAM");
        auto* decoded=JKRAram::aramToMainRam(resource,nullptr,0,0,Switch_1,0,root,3,&transferred);
        require(decoded && transferred==expanded.size() && !std::memcmp(decoded,expanded.data(),expanded.size()),
            "real title archive expands from ARAM into game memory");
        require(JKRExpHeap::CMemBlock::getHeapBlock(decoded)->getGroupId()==3,"main-memory group id uses the native heap header");
        root->free(decoded);
        u8 prefix[80]; std::memset(prefix,0x5a,sizeof(prefix));
        require(JKRAram::aramToMainRam(resource,prefix,0,0,Switch_1,64,root,-1,&transferred)==prefix &&
            transferred==64 && !std::memcmp(prefix,expanded.data(),64) && prefix[64]==0x5a,"bounded expansion preserves caller guard bytes");
        require(JKRDecompressFromAramToMainRam(resource->mAddress,prefix,resource->mSize,64,23,&transferred)==0 &&
            transferred==64 && !std::memcmp(prefix,expanded.data()+23,64),"ARAM streaming decoder supports decoded offsets");
        require(!JKRAram::aramToMainRam(resource,prefix,1,resource->mSize,Switch_0,0,root,-1,&transferred) && !transferred,
            "block-relative reads reject out-of-range offsets");
        delete resource;
        {
            const char* path="user/Ebisawa/title/title.szs";
            auto* loaded=JKRDvdAramRipper::loadToAram(path,0,Switch_0,0,0,&transferred);
            require(loaded && transferred==((compressed.size()+31)&~size_t(31)),"original DVD-to-ARAM path wrapper caches padded file");
            decoded=JKRAram::aramToMainRam(loaded,nullptr,0,0,Switch_1,0,root,-1,&transferred);
            require(decoded && transferred==expanded.size() && !std::memcmp(decoded,expanded.data(),transferred),
                "DVD-to-ARAM-to-main-RAM title path preserves resource contents");
            root->free(decoded); delete loaded;
            const s32 entry=DVDConvertPathToEntrynum(const_cast<char*>(path));
            loaded=JKRDvdAramRipper::loadToAram(entry,0,Switch_1,0,64,&transferred);
            require(loaded && transferred==64 && p2_aram_read(loaded->mAddress,prefix,64) &&
                !std::memcmp(prefix,expanded.data(),64),"entry wrapper expands bounded title prefix into ARAM");
            delete loaded;
            JKRDvdFile file(path); callbackAddress=0;
            auto* command=JKRDvdAramRipper::loadToAram_Async(&file,0,Switch_1,completed,17,64,&transferred);
            require(command && callbackAddress==reinterpret_cast<uintptr_t>(command) &&
                JKRDvdAramRipper::syncAram(command,TRUE) && transferred==64,"native ARAM callback and completion");
            require(p2_aram_read(command->mBlock->mAddress,prefix,64) && !std::memcmp(prefix,expanded.data()+17,64),
                "compressed DVD offset is decoded offset");
            delete command->mBlock; delete command;
            loaded=heap->alloc(128,JKRAramHeap::AM_Head);
            u8 guard[128]; std::memset(guard,0x5a,sizeof(guard)); p2_aram_write(loaded->mAddress,guard,sizeof(guard));
            require(JKRDvdAramRipper::loadToAram(&file,loaded->mAddress,Switch_0,32,128,&transferred)==
                reinterpret_cast<JKRAramBlock*>(UINTPTR_MAX) && transferred==96,"raw file limit is applied before offset");
            p2_aram_read(loaded->mAddress,guard,sizeof(guard));
            require(!std::memcmp(guard,compressed.data()+32,96) && guard[96]==0x5a,"explicit-address write stays within raw transfer size");
            require(JKRDecompressFromDVDToAram(&file,loaded->mAddress,compressed.size(),64,51,0,&transferred)==0 &&
                transferred==64 && p2_aram_read(loaded->mAddress,prefix,64) && !std::memcmp(prefix,expanded.data()+51,64),
                "direct DVD decompressor writes ARAM at the requested decoded offset");
            p2_aram_read(loaded->mAddress,guard,sizeof(guard));
            JKRADCommand bounded;
            bounded.mDvdFile=&file; bounded.mBlock=loaded; bounded.mExpandSwitch=Switch_1; bounded.mSizePtr=&transferred;
            require(!JKRDvdAramRipper::callCommand_Async(&bounded) && bounded.mStatus<0 && transferred==0,
                "existing ARAM block rejects a transfer larger than its allocation");
            u8 unchanged[128]; p2_aram_read(loaded->mAddress,unchanged,sizeof(unchanged));
            require(!std::memcmp(unchanged,guard,sizeof(guard)),"failed transfer does not overwrite the caller's ARAM allocation");
            delete loaded;
            require(!JKRDvdAramRipper::loadToAram(&file,0,Switch_0,UINT32_MAX,0,&transferred) && transferred==0,
                "invalid DVD offset fails without allocating an ARAM block");
            transferred=123;
            require(!JKRDvdAramRipper::loadToAram("missing-aram-resource",0,Switch_0,0,0,&transferred) && transferred==0,
                "missing DVD file clears transfer count");
        }
        require(heap->getFreeSize()==free,"resource release returns all ARAM space");
        delete manager;
        require(!ARCheckInit() && !JKRAram::sAramObject && !JKRAramHeap::sAramList.getNumLinks(),"manager teardown removes all block links");
        if(!round) baseline=root->getTotalFreeSize();
        else require(root->getTotalFreeSize()==baseline,"repeated ARAM lifetimes release native game-heap metadata");
    }
    std::puts("Native ARAM: boot partitions, original allocator, bounded transfers, native callbacks, 2,500 concurrent allocations and DVD/ARAM/title expansion pass.");
}
