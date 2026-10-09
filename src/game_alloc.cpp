#include "JSystem/JKernel/JKRHeap.h"
#include "p2_game_alloc.h"
#include <cstdlib>
#include <climits>
#include <cstdio>
#include <malloc/malloc.h>


void* p2_game_alloc(size_t bytes, int alignment, JKRHeap* heap) {
    if (bytes > UINT32_MAX || alignment == INT_MIN) return nullptr;
    if (!heap && !p2_host_scratch_active()) heap = JKRHeap::sCurrentHeap;
    if (heap) return heap->alloc(static_cast<u32>(bytes), alignment);
    // Native C++ runtime allocations can occur before the game arena exists.
    size_t align = alignment < 0 ? -alignment : alignment;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (align & (align-1)) return nullptr;
    void* memory = nullptr;
    return posix_memalign(&memory, align, bytes ? bytes : 1) == 0 ? memory : nullptr;
}
void p2_game_free(void* memory) {
    if (!memory) return;
    if (JKRHeap* heap = JKRHeap::findFromRoot(memory)) heap->free(memory);
    // Host allocations (no arena yet, or host scratch) go back to malloc. The
    // console ignored frees no heap owns, and the game relies on that (double
    // deletes in failure paths), so anything else is reported, not freed.
    else if (malloc_size(memory)) std::free(memory);
    else {
        static int reports;
        if (reports++ < 8) std::fprintf(stderr, "*** free of %p ignored: no game heap or malloc block owns it\n", memory);
    }
}
