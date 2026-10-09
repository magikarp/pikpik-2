#include "p2_host_compat.h"
#include "p2_memory.h"
#include "p2_panic.h"
#include "Dolphin/os.h"
#include <pthread.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

// The prepared SDK uses a native symbol for this fixed hardware clock. Aurora
// also publishes the same value in the shared low-memory boot area.
extern "C" { u32 __OSBusClock = 162000000; }

namespace {
unsigned char* memoryBase;
uint32_t memorySize;
void* arenaLo;
void* arenaHi;
struct MutexEntry { void* key; pthread_mutex_t mutex; MutexEntry* next; };
pthread_mutex_t registryLock = PTHREAD_MUTEX_INITIALIZER;
MutexEntry* mutexes;
struct CondEntry {
    void* key;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint64_t generation;
    unsigned waiters;
    CondEntry* next;
};
CondEntry* conditions;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "Pikmin 2 memory service: %s\n", message);
    std::abort();
}
void check(int result) { if (result) fail("pthread operation failed"); }
MutexEntry* findMutex(void* key) {
    check(pthread_mutex_lock(&registryLock));
    auto* found = mutexes;
    while (found && found->key != key) found = found->next;
    check(pthread_mutex_unlock(&registryLock));
    if (!found) fail("use of an uninitialized OS mutex");
    return found;
}
CondEntry* findCond(void* key) {
    check(pthread_mutex_lock(&registryLock));
    auto* found=conditions;
    while(found && found->key!=key) found=found->next;
    check(pthread_mutex_unlock(&registryLock));
    if(!found) fail("use of an uninitialized OS condition");
    return found;
}
bool inMemory(const void* pointer) {
    auto p = reinterpret_cast<uintptr_t>(pointer);
    auto base = reinterpret_cast<uintptr_t>(memoryBase);
    return memoryBase && p >= base && p-base <= memorySize;
}
}

extern "C" int p2_memory_init(uint32_t bytes) {
    if (memoryBase) return bytes == memorySize;
    if (bytes < 1024*1024 || bytes > 0x7fffffff || (bytes & 0xfff)) return 0;
    void* mapped = mmap(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) return 0;
    memoryBase = static_cast<unsigned char*>(mapped);
    memorySize = bytes;
    arenaLo = memoryBase + 0x4000;
    arenaHi = memoryBase + bytes;
    auto* boot = reinterpret_cast<OSBootInfo*>(memoryBase);
    boot->magic = OS_BOOTINFO_MAGIC;
    boot->memorySize = bytes;
    boot->arenaLo = arenaLo;
    boot->arenaHi = arenaHi;
    return 1;
}
extern "C" void* p2_physical_to_host(uint32_t address) {
    if (!memoryBase || address > memorySize) fail("physical address outside MEM1");
    return memoryBase + address;
}
extern "C" uint32_t p2_host_to_physical(const void* address) {
    if (!inMemory(address)) fail("graphics/ARAM address outside MEM1");
    return static_cast<uint32_t>(static_cast<const unsigned char*>(address)-memoryBase);
}
extern "C" void* OSGetArenaLo() { return arenaLo; }
extern "C" void* OSGetArenaHi() { return arenaHi; }
extern "C" void OSSetArenaLo(void* address) {
    if (!inMemory(address)) fail("arena low outside MEM1");
    arenaLo = address;
}
extern "C" void OSSetArenaHi(void* address) {
    if (!inMemory(address)) fail("arena high outside MEM1");
    arenaHi = address;
}
extern "C" void* OSInitAlloc(void* low, void* high, int maxHeaps) {
    // Reserve native heap-descriptor storage as the SDK does. JKR takes the
    // remaining arena and manages its own allocations, not OS heap handles.
    struct Descriptor { void* free; void* used; int64_t size; };
    if (!inMemory(low) || !inMemory(high) || low >= high || maxHeaps < 1) return nullptr;
    const auto count = static_cast<uint64_t>(maxHeaps)*sizeof(Descriptor);
    const auto available = static_cast<unsigned char*>(high)-static_cast<unsigned char*>(low);
    if (count+32 >= static_cast<uint64_t>(available)) return nullptr;
    std::memset(low, 0, count);
    return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(low)+count+31)&~uintptr_t(31));
}
extern "C" void OSInitMutex(OSMutex* mutex) {
    check(pthread_mutex_lock(&registryLock));
    auto* entry = mutexes;
    while (entry && entry->key != mutex) entry = entry->next;
    if (entry) check(pthread_mutex_destroy(&entry->mutex));
    else {
        entry = static_cast<MutexEntry*>(std::calloc(1, sizeof(MutexEntry)));
        if (!entry) fail("mutex allocation failed");
        entry->key = mutex; entry->next = mutexes; mutexes = entry;
    }
    pthread_mutexattr_t attributes;
    check(pthread_mutexattr_init(&attributes));
    check(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
    check(pthread_mutex_init(&entry->mutex, &attributes));
    check(pthread_mutexattr_destroy(&attributes));
    *mutex = OSMutex{};
    check(pthread_mutex_unlock(&registryLock));
}
extern "C" void OSLockMutex(OSMutex* mutex) {
    auto& native=findMutex(mutex)->mutex;
    if(pthread_mutex_trylock(&native)) { // contended: waiting, not working
        p2_worker_idle_begin(); check(pthread_mutex_lock(&native)); p2_worker_idle_end();
    }
    ++mutex->count;
}
extern "C" void OSUnlockMutex(OSMutex* mutex) {
    --mutex->count; check(pthread_mutex_unlock(&findMutex(mutex)->mutex));
}
// On the single-core console a try-lock by the running thread finds the mutex
// held only if its holder is blocked mid-operation; a thread it just signalled
// cannot run first. Here that thread starts on another core and holds the mutex
// for a moment (the card thread re-checking its queue), and the memory card
// code asserts when the try fails (ebiFileSelectMgr.cpp:90, seen on iPad). So a
// busy mutex is waited for, up to 100 ms, before the try fails. Only the card
// code (and the unused e-Reader) calls this.
extern "C" BOOL OSTryLockMutex(OSMutex* mutex) {
    pthread_mutex_t* native = &findMutex(mutex)->mutex;
    int result = pthread_mutex_trylock(native);
    if (result == EBUSY) {
        timespec start{}, now{};
        clock_gettime(CLOCK_MONOTONIC, &start);
        p2_worker_idle_begin();
        do {
            const timespec pause{0, 50000};
            nanosleep(&pause, nullptr);
            result = pthread_mutex_trylock(native);
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (result == EBUSY &&
                 (now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) < 100000000L);
        p2_worker_idle_end();
        if (result == EBUSY) return FALSE;
    }
    check(result); ++mutex->count; return TRUE;
}
extern "C" void p2_destroy_mutex(void* mutex) {
    check(pthread_mutex_lock(&registryLock));
    auto** link = &mutexes;
    while (*link && (*link)->key != mutex) link = &(*link)->next;
    if (*link) {
        auto* entry = *link;
        check(pthread_mutex_destroy(&entry->mutex));
        *link = entry->next; std::free(entry);
    }
    check(pthread_mutex_unlock(&registryLock));
}
extern "C" void OSReport(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
extern "C" void p2_heap_report(const char* text) {
    std::fputs(text, stderr);
}
extern "C" void p2_heap_reportf(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
extern "C" void OSPanic(const char* file, int line, const char* format, ...) {
    va_list args; va_start(args, format);
    p2_panic_v(file,line,format,&args);
}

extern "C" void OSInitCond(OSCond* key) {
    check(pthread_mutex_lock(&registryLock));
    auto* entry=conditions;
    while(entry && entry->key!=key) entry=entry->next;
    if(entry) {
        check(pthread_mutex_lock(&entry->mutex));
        if(entry->waiters) fail("reinitializing a condition with active waiters");
        entry->generation=0;
        check(pthread_mutex_unlock(&entry->mutex));
    } else {
        entry=static_cast<CondEntry*>(std::calloc(1,sizeof(CondEntry)));
        if(!entry) fail("condition allocation failed");
        entry->key=key;
        check(pthread_mutex_init(&entry->mutex,nullptr));
        check(pthread_cond_init(&entry->changed,nullptr));
        entry->next=conditions; conditions=entry;
    }
    *key=OSCond{};
    check(pthread_mutex_unlock(&registryLock));
}
extern "C" void OSWaitCond(OSCond* key,OSMutex* mutex) {
    auto* condition=findCond(key);
    auto* lock=findMutex(mutex);
    // The caller holds the recursive game mutex. Register the waiter before
    // dropping it, including every recursive acquisition. The condition's own
    // lock prevents a signal between releasing the game lock and sleeping.
    const int depth=mutex->count;
    if(depth<=0) fail("condition wait without a locked mutex");
    check(pthread_mutex_lock(&condition->mutex));
    const uint64_t generation=condition->generation;
    ++condition->waiters;
    mutex->count=0;
    for(int i=0;i<depth;++i) check(pthread_mutex_unlock(&lock->mutex));
    // An idle condition wait is a safe cancellation point even inside a work
    // scope: the game mutex is released and the waiter is not mid-operation.
    // SDK OSCancelThread stops such a thread outright (e.g. a task's service
    // loop parked here forever). Never while holding the interrupt gate.
    const auto cancelled=[] { return p2_thread_cancel_requested() && !p2_critical_section_held(); };
    bool stopped=false;
    while(generation==condition->generation && !(stopped=cancelled())) {
        timespec deadline{}; clock_gettime(CLOCK_REALTIME,&deadline);
        deadline.tv_nsec+=50000000;
        if(deadline.tv_nsec>=1000000000) { ++deadline.tv_sec; deadline.tv_nsec-=1000000000; }
        p2_worker_idle_begin();
        const int result=pthread_cond_timedwait(&condition->changed,&condition->mutex,&deadline);
        p2_worker_idle_end();
        if(result!=ETIMEDOUT) check(result);
    }
    --condition->waiters;
    check(pthread_mutex_unlock(&condition->mutex));
    if(stopped) OSExitThread(nullptr); // Outside all condition/game locks.
    for(int i=0;i<depth;++i) check(pthread_mutex_lock(&lock->mutex));
    mutex->count=depth;
}
extern "C" void OSSignalCond(OSCond* key) {
    auto* condition=findCond(key);
    check(pthread_mutex_lock(&condition->mutex));
    ++condition->generation;
    // SDK OSWakeupThread wakes the entire queue, not just one waiter.
    check(pthread_cond_broadcast(&condition->changed));
    check(pthread_mutex_unlock(&condition->mutex));
}
extern "C" void p2_destroy_condition(void* key) {
    check(pthread_mutex_lock(&registryLock));
    auto** link=&conditions;
    while(*link && (*link)->key!=key) link=&(*link)->next;
    if(*link) {
        auto* entry=*link;
        check(pthread_mutex_lock(&entry->mutex));
        if(entry->waiters) fail("destroying a condition with active waiters");
        check(pthread_mutex_unlock(&entry->mutex));
        check(pthread_cond_destroy(&entry->changed));
        check(pthread_mutex_destroy(&entry->mutex));
        *link=entry->next; std::free(entry);
    }
    check(pthread_mutex_unlock(&registryLock));
}
