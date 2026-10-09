// Compiled with Aurora headers only. The native game owns MEM1 and its arena;
// Aurora's address translation and GX decoder must observe that same allocation.
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include "p2_memory.h"
#include <cstdio>
#include <cstdlib>
namespace aurora { extern AuroraConfig g_config; }
uintptr_t OSBaseAddress = 0;
void* MEM1Start = nullptr;
void* MEM1End = nullptr;

void AuroraOSInitMemory() {
    const auto bytes = aurora::g_config.mem1Size;
    if (!bytes || !p2_memory_init(bytes)) {
        std::fputs("Pikmin 2: renderer MEM1 size is missing or conflicts with the game arena\n",stderr);
        std::abort();
    }
    MEM1Start = p2_physical_to_host(0);
    MEM1End = p2_physical_to_host(bytes);
    OSBaseAddress = reinterpret_cast<uintptr_t>(MEM1Start);
}
void AuroraInitArena() {
    // p2_memory_init sets boundaries exactly once. Never reset them here: JKR
    // may already have claimed the arena before OSInit is called again.
}
u32 OSGetPhysicalMemSize() { return aurora::g_config.mem1Size; }

extern "C" void* OSAllocFromArenaLo(u32 size,u32 align) {
    if (!align || (align & (align-1))) return nullptr;
    const uintptr_t low=reinterpret_cast<uintptr_t>(OSGetArenaLo());
    const uintptr_t high=reinterpret_cast<uintptr_t>(OSGetArenaHi());
    const uintptr_t start=(low+align-1)&~uintptr_t(align-1);
    const uintptr_t end=(start+size+align-1)&~uintptr_t(align-1);
    if (start<low || end<start || end>high) return nullptr;
    OSSetArenaLo(reinterpret_cast<void*>(end));
    return reinterpret_cast<void*>(start);
}
extern "C" void* OSAllocFromArenaHi(u32 size,u32 align) {
    if (!align || (align & (align-1))) return nullptr;
    const uintptr_t low=reinterpret_cast<uintptr_t>(OSGetArenaLo());
    const uintptr_t high=reinterpret_cast<uintptr_t>(OSGetArenaHi())&~uintptr_t(align-1);
    if(high<low || size>high-low) return nullptr;
    const uintptr_t start=(high-size)&~uintptr_t(align-1);
    if(start<low) return nullptr;
    OSSetArenaHi(reinterpret_cast<void*>(start));
    return reinterpret_cast<void*>(start);
}
