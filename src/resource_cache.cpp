#include "ARAM.h"
#include "Pikmin2ARAM.h"
#include "JSystem/JKernel/JKRDvdAramRipper.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include <cctype>
#include <cstring>
#include <vector>

ARAM::Mgr* gAramMgr;
namespace {
struct Lock {
    OSMutex* mutex;
    explicit Lock(OSMutex* value):mutex(value) { OSLockMutex(mutex); }
    ~Lock() { OSUnlockMutex(mutex); }
};
}
namespace ARAM {
Node::Node():CNode(const_cast<char*>("")),mMemoryBlock(nullptr) {}
Node::~Node() {
    del(); delete mMemoryBlock;
    if(mNativeOwnName) delete[] mName;
}
u32 Node::dvdToAram(const char* name,bool entryOnly) {
    if(!name) return 0;
    mName=name;
    if(!mMemoryBlock && !entryOnly) mMemoryBlock=JKRDvdAramRipper::loadToAram(name,0,Switch_0,0,0,nullptr);
    return mMemoryBlock!=nullptr;
}
void* Node::aramToMainRam(u8* buffer,u32 length,u32 offset,JKRExpandSwitch expand,u32 capacity,JKRHeap* heap,
    JKRDvdRipper::EAllocDirection direction,int id,u32* written) {
    u32 count=0; if(written) *written=0;
    if(!mMemoryBlock) dvdToAram(mName,false);
    if(!mMemoryBlock) return nullptr;
    auto* result=JKRAram::aramToMainRam(mMemoryBlock,buffer,length,offset,expand,capacity,heap,id,&count);
    if(!result) return nullptr;
    // A caller's buffer is borrowed regardless of allocation direction.
    if(!buffer && direction==JKRDvdRipper::ALLOC_DIR_BOTTOM) {
        auto* tail=static_cast<u8*>(JKRHeap::alloc(count ? count : 1,-32,heap));
        if(!tail) { JKRHeap::free(result,nullptr); return nullptr; }
        if(count) std::memcpy(tail,result,count);
        JKRHeap::free(result,nullptr); result=tail;
        JKRAram::changeGroupIdIfNeed(result,id);
    }
    if(written) *written=count;
    return result;
}
Mgr::Mgr():mResourceList(const_cast<char*>("root")) {
    OSInitMutex(&mNativeMutex);
    if(gAramMgr) OSPanic(__FILE__,__LINE__,"ARAM resource cache already exists");
    gAramMgr=this;
}
Mgr::~Mgr() {
    {
        Lock lock(&mNativeMutex);
        while(mResourceList.mChild) delete static_cast<Node*>(mResourceList.mChild);
        if(gAramMgr==this) gAramMgr=nullptr;
    }
    p2_destroy_mutex(&mNativeMutex);
}
void Mgr::init() { if(!gAramMgr) new Mgr; }
Node* Mgr::search(const char* name) {
    if(!name) return nullptr;
    Lock lock(&mNativeMutex);
    for(auto* node=mResourceList.mChild;node;node=node->mNext)
        if(!std::strcmp(name,node->mName)) return static_cast<Node*>(node);
    return nullptr;
}
u32 Mgr::dvdToAram(const char* name,bool entryOnly) {
    if(!name || !*name) return 0;
    Lock lock(&mNativeMutex);
    if(auto* node=search(name)) return node->dvdToAram(node->mName,entryOnly);
    auto* node=new (JKRHeap::sSystemHeap,0) Node;
    if(!node) return 0;
    auto* copy=new (JKRHeap::sSystemHeap,0) char[std::strlen(name)+1];
    if(!copy) { delete node; return 0; }
    std::strcpy(copy,name); node->mName=copy; node->mNativeOwnName=true;
    const auto loaded=node->dvdToAram(copy,entryOnly);
    if(loaded || entryOnly) mResourceList.add(node);
    else delete node;
    return loaded;
}
void* Mgr::aramToMainRam(const char* name,u8* buffer,u32 length,u32 offset,JKRExpandSwitch expand,u32 capacity,JKRHeap* heap,
    JKRDvdRipper::EAllocDirection direction,int id,u32* written) {
    if(written) *written=0;
    Lock lock(&mNativeMutex); auto* node=search(name);
    return node ? node->aramToMainRam(buffer,length,offset,expand,capacity,heap,direction,id,written) : nullptr;
}
void Mgr::dump() {
    Lock lock(&mNativeMutex);
    p2_heap_reportf("ARAM resource cache: %d entries, %d bytes in largest free block\n",
        mResourceList.getChildCount(),JKRAram::getAramHeap()->getFreeSize());
}
}

void Pikmin2ARAM::Mgr::loadEnemy() {
    P2HostScratchScope scratch;
    // The console carved temporary heaps around a 30 KiB parser workspace.
    // Native metadata is larger; read this plain token list with explicit bounds.
    JKRDvdFile file("/enemy/enemyResList.txt");
    if(!file.mFileOpen || file.getFileSize()>1024*1024) OSPanic(__FILE__,__LINE__,"Invalid enemy resource list");
    const auto length=file.getFileSize();
    std::vector<u8> bytes(size_t(length)+1,0);
    if(file.readData(bytes.data(),length,0)!=s32(length)) OSPanic(__FILE__,__LINE__,"Cannot read enemy resource list");
    size_t position=0;
    while(position<length) {
        while(position<length && std::isspace(bytes[position])) ++position;
        if(position==length) break;
        const auto start=position;
        while(position<length && !std::isspace(bytes[position])) ++position;
        bytes[position]=0;
        const char* name=reinterpret_cast<char*>(bytes.data()+start);
        if(!std::strcmp(name,"EOF")) return;
        if(*name!='/' || position-start>=1024) OSPanic(__FILE__,__LINE__,"Invalid enemy resource path");
        gAramMgr->dvdToAram(name,!mLoadPermission);
        ++position;
    }
    OSPanic(__FILE__,__LINE__,"Enemy resource list has no EOF marker");
}
