#include "JSystem/JKernel/JKRHeap.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int failures;
int addrToXPos(void*,int);
static void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static void* worker(void* argument) {
    auto* heap = static_cast<JKRHeap*>(argument);
    for (int i=0; i<1000; ++i) {
        auto* allocation = static_cast<unsigned char*>(heap->alloc(17+i%97, 32));
        if (!allocation) return reinterpret_cast<void*>(1);
        std::memset(allocation, i & 255, 17+i%97);
        for (int j=0; j<17+i%97; ++j)
            if (allocation[j] != (i & 255)) return reinterpret_cast<void*>(1);
        heap->free(allocation);
    }
    return nullptr;
}
struct Disposable : JKRDisposer {
    explicit Disposable(int* destroyed) : destroyed(destroyed) {}
    ~Disposable() override { ++*destroyed; }
    int* destroyed;
};
int main() {
    check(p2_memory_init(64*1024*1024), "native MEM1 arena");
    auto* root = JKRExpHeap::createRoot(16, false);
    if (!root) return 1;
    check(JKRExpHeap::createRoot(16, false) == root, "root creation is idempotent");
    const auto initialFree = root->getTotalFreeSize();
    check(addrToXPos(p2_physical_to_host(0),640)==0 &&
          addrToXPos(p2_physical_to_host(JKRHeap::getMemorySize()/2),640)==320 &&
          addrToXPos(p2_physical_to_host(JKRHeap::getMemorySize()),640)==640,
          "heap display maps native MEM1 addresses including its end boundary");
    check(reinterpret_cast<uintptr_t>(root) > UINT32_MAX, "exercise real 64-bit heap addresses");
    void* allocations[128] = {};
    for (int i=0; i<128; ++i) {
        const int alignment = 1 << (3+i%6);
        const int size = 1+i*3;
        allocations[i] = root->alloc(size, i%2 ? alignment : -alignment);
        check(allocations[i] && !(reinterpret_cast<uintptr_t>(allocations[i]) & (alignment-1)), "head/tail alignment");
        if (!allocations[i]) return 1;
        check(JKRHeap::findFromRoot(allocations[i]) == root, "native heap ownership");
        check(p2_physical_to_host(p2_host_to_physical(allocations[i])) == allocations[i], "MEM1 address round-trip");
        std::memset(allocations[i], i, size);
    }
    for (int i=0; i<128; ++i) {
        for (int j=0; j<1+i*3; ++j)
            check(static_cast<unsigned char*>(allocations[i])[j] == i, "allocations do not overlap");
    }
    for (int i=0; i<128; i+=2) root->free(allocations[i]);
    for (int i=127; i>=0; i-=2) root->free(allocations[i]);
    check(root->check() && root->getTotalFreeSize() == initialFree, "coalescing restores free space");
    auto* resized = root->alloc(64, 16);
    std::memset(resized, 0x5a, 64);
    check(root->resize(resized, 256) >= 256, "allocation grows into adjacent free block");
    check(root->resize(resized, 32) >= 32, "allocation shrinks");
    for (int i=0; i<32; ++i) check(static_cast<unsigned char*>(resized)[i] == 0x5a, "resize retains bytes");
    root->free(resized);
    for (int alignment : {16, 32, 64, 256}) {
        const u32 maximum = root->getMaxAllocatableSize(alignment);
        void* largest = root->alloc(maximum, alignment);
        check(largest != nullptr, "reported maximum allocation fits native block headers");
        root->free(largest);
    }
    auto* cpp = new char[31];
    check(JKRHeap::findFromRoot(cpp) == root, "plain game new uses the arena");
    delete[] cpp;
    char* scratch;
    {
        P2HostScratchScope outer;
        {
            P2HostScratchScope inner;
            scratch = new char[47];
            check(scratch && !JKRHeap::findFromRoot(scratch), "nested native scratch uses host memory");
        }
        auto* explicitGame = new (root,32) char[47];
        check(explicitGame && JKRHeap::findFromRoot(explicitGame)==root, "explicit game allocation retains ownership inside scratch scope");
        delete[] explicitGame;
        auto* stillHost = new char[47];
        check(stillHost && !JKRHeap::findFromRoot(stillHost), "inner scope leaves outer scratch routing active");
        delete[] stillHost;
    }
    delete[] scratch;
    cpp = new char[31];
    check(JKRHeap::findFromRoot(cpp)==root, "scratch scope restores default game allocation routing");
    delete[] cpp;
    auto* child = JKRExpHeap::create(1024*1024, root, false);
    check(child != nullptr, "child expanded heap");
    int destroyed = 0;
    new (child, 16) Disposable(&destroyed);
    child->destroy();
    check(destroyed == 1, "child heap invokes registered virtual destructors");
    auto* solid = JKRSolidHeap::create(1024*1024, root, false);
    check(solid != nullptr, "child solid heap");
    auto* head = solid->alloc(53, 32);
    auto* tail = solid->alloc(71, -64);
    check(head && tail && head < tail && !(reinterpret_cast<uintptr_t>(tail)&63), "solid head/tail allocation");
    solid->destroy();
    pthread_t threads[4];
    for (auto& thread : threads) check(pthread_create(&thread, nullptr, worker, root) == 0, "start concurrent allocator worker");
    for (auto thread : threads) {
        void* result = nullptr;
        check(pthread_join(thread, &result) == 0 && !result, "concurrent allocations retain contents");
    }
    check(root->check() && root->getTotalFreeSize() == initialFree, "heap remains intact after child cleanup and concurrency");
    std::printf("Native JKR heap checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
