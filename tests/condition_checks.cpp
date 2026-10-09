#include "Dolphin/os.h"
#include "p2_memory.h"
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <unistd.h>
static OSMutex mutex;
static OSCond condition;
static unsigned ready,completed;
static bool released;
static std::atomic<int> failures{0};
static void check(bool ok,const char* text) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",text);++failures; } }
static void* waiter(void*) {
    OSLockMutex(&mutex); OSLockMutex(&mutex);
    ++ready;
    while(!released) OSWaitCond(&condition,&mutex);
    check(mutex.count==2,"recursive depth restored");
    ++completed;
    OSUnlockMutex(&mutex); OSUnlockMutex(&mutex);
    return nullptr;
}
static std::atomic<bool> cancelReady{false};
static std::atomic<unsigned> unexpectedWakeups{0};
static void* cancelWaiter(void*) {
    OSLockMutex(&mutex); OSLockMutex(&mutex);
    cancelReady=true;
    for(;;) { OSWaitCond(&condition,&mutex); ++unexpectedWakeups; }
}
int main() {
    OSInitMutex(&mutex); OSInitCond(&condition);
    for(unsigned round=0;round<30;++round) {
        ready=completed=0;released=false;
        pthread_t workers[4];
        for(auto& worker:workers) check(!pthread_create(&worker,nullptr,waiter,nullptr),"start condition waiter");
        for(;;) {
            OSLockMutex(&mutex); const bool all=ready==4;
            if(all) released=true;
            OSUnlockMutex(&mutex);
            if(all) break;
            usleep(1000);
        }
        // The game signals after dropping its mutex. All registered waiters
        // must wake from this single signal, including their recursion depth.
        OSSignalCond(&condition);
        for(auto& worker:workers) check(!pthread_join(worker,nullptr),"join condition waiter");
        check(completed==4,"single signal wakes every registered waiter");
        OSInitCond(&condition);
    }
    OSSignalCond(&condition); // Conditions do not retain a signal for future waiters.
    OSThread thread{}; alignas(32) unsigned char stack[4096];
    check(OSCreateThread(&thread,cancelWaiter,nullptr,stack+sizeof(stack),sizeof(stack),16,0),"create cancellable waiter");
    OSResumeThread(&thread);
    while(!cancelReady) usleep(1000);
    OSLockMutex(&mutex); // Acquired only after the waiter releases both levels.
    check(unexpectedWakeups==0,"earlier signal is not retained for a future wait");
    OSUnlockMutex(&mutex);
    OSCancelThread(&thread);
    check(OSTryLockMutex(&mutex),"cancelled wait releases all recursive locks");
    check(mutex.count==1,"cancelled wait clears recursive metadata");
    OSUnlockMutex(&mutex);
    p2_destroy_thread(&thread);
    p2_destroy_condition(&condition); p2_destroy_mutex(&mutex);
    // Reusing an address must allocate fresh metadata, with no stale waiters.
    OSInitMutex(&mutex); OSInitCond(&condition); OSSignalCond(&condition);
    p2_destroy_condition(&condition); p2_destroy_mutex(&mutex);
    std::printf("Native condition checks: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
