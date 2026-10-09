#include "p2_memory.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "Dolphin/os.h"
#include <cstdio>
extern "C" {
void p2_test_renderer_os_init(unsigned);
void* p2_test_renderer_physical(unsigned);
unsigned p2_test_renderer_address(void*);
unsigned p2_test_renderer_size();
unsigned p2_test_renderer_clock();
void* p2_test_arena_low(unsigned,unsigned);
void* p2_test_arena_high(unsigned,unsigned);
}
static int failures;
static void check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
int main(int argc,char**) {
    const unsigned bytes=128*1024*1024;
    JKRExpHeap* root=nullptr;
    if(argc>1) {
        check(p2_memory_init(bytes),"game memory init");
        root=JKRExpHeap::createRoot(16,false);
        check(root!=nullptr,"heap before renderer");
    }
    void* savedLow=OSGetArenaLo(); void* savedHigh=OSGetArenaHi();
    p2_test_renderer_os_init(bytes);
    if(argc>1) check(savedLow==OSGetArenaLo()&&savedHigh==OSGetArenaHi(),"renderer preserves claimed arena");
    check(p2_test_renderer_size()==bytes,"renderer memory extent");
    check(p2_test_renderer_clock()==162000000&&__OSBusClock==p2_test_renderer_clock(),"game and renderer clock agreement");
    check(p2_test_renderer_physical(0)==p2_physical_to_host(0),"single MEM1 backing");
    check(p2_test_renderer_physical(bytes)==p2_physical_to_host(bytes),"shared end boundary");
    if(!root) {
        void* low=p2_test_arena_low(113,64); void* high=p2_test_arena_high(71,64);
        check(low&&high&&!(reinterpret_cast<uintptr_t>(low)&63)&&!(reinterpret_cast<uintptr_t>(high)&63),"shared arena aligned allocations");
        savedLow=OSGetArenaLo(); savedHigh=OSGetArenaHi();
        check(!p2_test_arena_low(bytes,64)&&!p2_test_arena_high(bytes,64)&&!p2_test_arena_low(1,3),"arena rejects overflow and invalid alignment");
        check(savedLow==OSGetArenaLo()&&savedHigh==OSGetArenaHi(),"failed allocation preserves boundaries");
        root=JKRExpHeap::createRoot(16,false);
    }
    check(root!=nullptr,"real JKR root");
    if(root) {
        void* allocation=root->alloc(8192,32);
        check(allocation!=nullptr,"real JKR graphics allocation");
        if(allocation) {
            const unsigned offset=p2_host_to_physical(allocation);
            check(p2_test_renderer_address(allocation)==offset,"game and renderer encode same physical offset");
            check(p2_test_renderer_physical(offset)==allocation,"renderer resolves live game allocation");
            static_cast<unsigned char*>(allocation)[123]=0x5a;
            check(static_cast<unsigned char*>(p2_test_renderer_physical(offset))[123]==0x5a,"renderer reads game allocation bytes");
            savedLow=OSGetArenaLo();savedHigh=OSGetArenaHi();p2_test_renderer_os_init(bytes);
            check(savedLow==OSGetArenaLo()&&savedHigh==OSGetArenaHi(),"repeated OSInit leaves heap intact");
            root->free(allocation);
        }
    }
    std::printf("Shared renderer/game MEM1: %s\n",failures?"FAILED":"passed");
    return failures?1:0;
}
