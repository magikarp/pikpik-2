#pragma once
#include <stddef.h>
struct JKRHeap;
void* p2_game_alloc(size_t bytes, int alignment, JKRHeap* heap);
void p2_game_free(void* memory);

// Native loader scratch storage must not exhaust a selected console heap.
// Explicit heap allocations and JKRHeap::alloc retain their normal behavior.
bool p2_host_scratch_active();
class P2HostScratchScope {
public:
    P2HostScratchScope();
    ~P2HostScratchScope();
    P2HostScratchScope(const P2HostScratchScope&) = delete;
    P2HostScratchScope& operator=(const P2HostScratchScope&) = delete;
};
