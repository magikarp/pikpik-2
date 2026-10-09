#include "Game/MemoryCard/Mgr.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char* message) { if(!ok) { std::fprintf(stderr,"Card command: %s\n",message); std::exit(1); } }
int main() {
    using namespace Game::MemoryCard;
    struct Queue { u64 before=0x1122334455667788ull; MemoryCardMgrCommand entries[5]; u64 after=0x8877665544332211ull; } queue;
    MemoryCardMgrCommand flags(3);
    MgrCommandPlayerNo player(8,2);
    MgrCommandCopyPlayer copy(12,1,2);
    alignas(void*) unsigned char infoStorage[sizeof(void*)]{};
    auto* info=reinterpret_cast<PlayerFileInfo*>(infoStorage);
    MgrCommandGetPlayerHeader header(13,info);
    MemoryCardMgrCommandBase* inputs[]={&flags,&player,&copy,&header,&flags};
    for(unsigned repeat=0;repeat<100;++repeat) for(unsigned slot=0;slot<5;++slot) {
        const auto type=(slot+repeat)%5;
        auto* command=inputs[type];
        require(command->getClassSize()<=sizeof(MemoryCardMgrCommand),"native queue capacity");
        command->copyTo(queue.entries[slot]);
        const auto& out=queue.entries[slot];
        require(out.mFlag==command->mFlag,"command flag preserved");
        if(type==1) require(out.mData.intView==2,"player number preserved");
        if(type==2) require(out.mData.shortView[0]==1 && out.mData.shortView[1]==2,"both player indices preserved");
        if(type==3) require(out.mData.dataView==info,"full native pointer preserved");
        if(type==0 || type==4) require(out.mData.dataView==nullptr,"plain command clears prior payload");
        require(queue.entries[slot].getClassSize()==sizeof(MemoryCardMgrCommand),"queue keeps its own vtable");
        require(queue.before==0x1122334455667788ull && queue.after==0x8877665544332211ull,"queue bounds intact");
    }
    std::puts("Native card command flags, scalar/pointer payloads and queue reuse passed.");
}
