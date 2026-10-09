#include "Dolphin/os.h"
#include "p2_memory.h"
#include <pthread.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

namespace {
// Serializes participating SDK critical sections. This does not disable host
// interrupts or preempt unrelated macOS threads; native services have their
// own locks and callbacks retain their documented native-thread ownership.
pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
thread_local bool interruptsEnabled=true,ownsGate=false;
thread_local s32 schedulerDepth=0;
thread_local int previousWork=0;
[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr,"Pikmin 2 critical section: %s\n",message);std::abort();
}
void acquire() {
    if(ownsGate) return;
    for(;;) {
        p2_thread_checkpoint();
        const int result=pthread_mutex_trylock(&gate);
        if(!result) break;
        if(result!=EBUSY) fail("lock failed");
        p2_worker_idle_begin(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); p2_worker_idle_end();
    }
    ownsGate=true;
    previousWork=p2_begin_thread_work();
}
void releaseIfIdle() {
    if(!ownsGate||!interruptsEnabled||schedulerDepth) return;
    ownsGate=false;
    if(pthread_mutex_unlock(&gate)) fail("unlock failed");
    // Only now may pending cancellation/suspension stop the owner.
    p2_end_thread_work(previousWork);
}
}
extern "C" int p2_critical_section_held() { return ownsGate; }
extern "C" void p2_critical_thread_exit() {
    // Explicit OSExitThread also relinquishes the software scheduling gate.
    if(ownsGate) {
        ownsGate=false;interruptsEnabled=true;schedulerDepth=0;
        if(pthread_mutex_unlock(&gate)) fail("thread-exit unlock failed");
    }
}
extern "C" BOOL OSDisableInterrupts() {
    const BOOL previous=interruptsEnabled;
    acquire();interruptsEnabled=false;return previous;
}
extern "C" BOOL OSEnableInterrupts() {
    const BOOL previous=interruptsEnabled;
    interruptsEnabled=true;releaseIfIdle();return previous;
}
extern "C" BOOL OSRestoreInterrupts(BOOL enabled) {
    return enabled ? OSEnableInterrupts() : OSDisableInterrupts();
}
extern "C" s32 OSDisableScheduler() {
    if(schedulerDepth==INT_MAX) fail("scheduler nesting overflow");
    acquire();return schedulerDepth++;
}
extern "C" s32 OSEnableScheduler() {
    if(!schedulerDepth) fail("unmatched scheduler enable");
    const s32 previous=schedulerDepth--;
    releaseIfIdle();return previous;
}
