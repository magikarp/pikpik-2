#include "Dolphin/os.h"
#include "Dolphin/gd.h"
#include "JSystem/J3D/J3DDisplayListObj.h"
#include "p2_memory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

static std::atomic<unsigned> failures{0};
static std::atomic<bool> started{false},releaseOwner{false},cleaned{false},returned{false};
static void require(bool ok,const char* message) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);++failures; }
}
static void* waiter(void*) {
    started=true;
    const BOOL mask=OSDisableInterrupts();returned=true;OSRestoreInterrupts(mask);return nullptr;
}
static void* owner(void*) {
    const BOOL mask=OSDisableInterrupts();started=true;
    while(!releaseOwner) OSYieldThread();
    cleaned=true;
    OSRestoreInterrupts(mask);
    returned=true;return nullptr;
}
static void* exitOwner(void*) {
    OSDisableInterrupts();OSDisableScheduler();OSExitThread(nullptr);return nullptr;
}
int main() {
    require(OSDisableInterrupts()==TRUE,"initial interrupt state");
    require(OSDisableInterrupts()==FALSE,"nested interrupt mask");
    require(OSRestoreInterrupts(FALSE)==FALSE,"inner restore retains mask");
    require(OSDisableScheduler()==0 && OSDisableScheduler()==1,"scheduler nesting counts");
    require(OSEnableInterrupts()==FALSE && p2_critical_section_held(),"scheduler retains gate after interrupts enabled");
    require(OSEnableScheduler()==2 && p2_critical_section_held(),"nested scheduler release retains gate");
    require(OSEnableScheduler()==1 && !p2_critical_section_held(),"outer scheduler release drops gate");

    require(OSDisableScheduler()==0,"scheduler-first acquisition");
    const BOOL nestedMask=OSDisableInterrupts();
    require(nestedMask==TRUE && OSEnableScheduler()==1 && p2_critical_section_held(),"interrupt mask retains gate after scheduler release");
    OSRestoreInterrupts(nestedMask);
    require(!p2_critical_section_held(),"last protection releases gate");

    std::thread workers[4];
    for(unsigned worker=0;worker<4;++worker) workers[worker]=std::thread([worker] {
        alignas(32) unsigned char storage[64];
        J3DDisplayListObj list;list.setSingleDisplayList(storage,sizeof(storage));
        for(unsigned cycle=0;cycle<500;++cycle) {
            const unsigned length=cycle%48+1;
            list.beginDL();
            for(unsigned byte=0;byte<length;++byte) {
                __GDWrite(static_cast<u8>(worker*59+cycle+byte));
                std::this_thread::yield();
            }
            require(list.endDL()==((length+31)&~31u),"concurrent original display-list length");
            for(unsigned byte=0;byte<length;++byte)
                require(storage[byte]==static_cast<u8>(worker*59+cycle+byte),"concurrent original display-list payload");
            for(unsigned byte=length;byte<list.getDisplayListSize();++byte)
                require(storage[byte]==0,"original display-list padding");
            list.beginPatch();__GDWrite(0x7d);
            require(list.endPatch()==((length+31)&~31u) && storage[0]==0x7d,"original patch preserves size");
        }
    });
    for(auto& worker:workers) worker.join();
    require(__GDCurrentDL==nullptr,"shared descriptor cleared after all lists");

    OSThread thread{};alignas(32) unsigned char stack[4096];
    auto spawn=[&](OSThreadStartFunction fn) {
        started=false;returned=false;
        require(OSCreateThread(&thread,fn,nullptr,stack+sizeof(stack),sizeof(stack),16,0),"managed worker creation");
        OSResumeThread(&thread);
    };
    const BOOL held=OSDisableInterrupts();
    spawn(waiter);while(!started) std::this_thread::yield();
    const auto time=std::chrono::steady_clock::now();OSCancelThread(&thread);
    require(!returned && std::chrono::steady_clock::now()-time<std::chrono::seconds(2),"blocked gate waiter cancels while another thread owns gate");
    p2_destroy_thread(&thread);OSRestoreInterrupts(held);

    spawn(owner);while(!started) std::this_thread::yield();
    OSSuspendThread(&thread);releaseOwner=true;
    // Acquisition proves that the owner completed cleanup and released its
    // gate before honoring suspension at the restore boundary.
    const BOOL probe=OSDisableInterrupts();
    require(cleaned && !returned,"suspension is deferred through owned critical section");
    OSRestoreInterrupts(probe);OSResumeThread(&thread);
    require(OSJoinThread(&thread,nullptr) && returned,"owner resumes after unlocked suspension");
    p2_destroy_thread(&thread);

    spawn(exitOwner);require(OSJoinThread(&thread,nullptr),"explicit exit joins");p2_destroy_thread(&thread);
    const BOOL afterExit=OSDisableInterrupts();OSRestoreInterrupts(afterExit);
    require(afterExit==TRUE,"explicit thread exit releases gate");
    std::printf("Native critical sections: 2000 original display lists/patches, nested masks, cancellation and suspension: %s\n",failures?"FAILED":"passed");
    return failures?1:0;
}
