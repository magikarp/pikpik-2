// OSTryLockMutex must not fail because another core holds the mutex for a moment
// (the memory card thread re-checking its queue; ebiFileSelectMgr.cpp:90 asserted
// on iPad), but must still fail when the holder keeps it.
#include "Dolphin/os.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
int main() {
    static OSMutex mutex;
    OSInitMutex(&mutex);
    std::atomic<bool> stop{false};
    // Holds the mutex for 0.2 ms at a time, like the card thread's queue check.
    std::thread holder([&] {
        while(!stop) {
            OSLockMutex(&mutex);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            OSUnlockMutex(&mutex);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    int failures=0;
    for(int i=0;i<500;++i) {
        if(OSTryLockMutex(&mutex)) OSUnlockMutex(&mutex); else ++failures;
    }
    stop=true; holder.join();
    require(failures==0,"try-lock waits out brief contention");

    std::atomic<bool> locked{false}, release{false};
    std::thread keeper([&] {
        OSLockMutex(&mutex); locked=true;
        while(!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        OSUnlockMutex(&mutex);
    });
    while(!locked) std::this_thread::yield();
    const auto start=std::chrono::steady_clock::now();
    require(!OSTryLockMutex(&mutex),"try-lock fails while the holder keeps the mutex");
    const auto waited=std::chrono::steady_clock::now()-start;
    require(waited<std::chrono::milliseconds(500),"failing try-lock is bounded");
    release=true; keeper.join();
    std::puts("try-lock checks passed");
}
