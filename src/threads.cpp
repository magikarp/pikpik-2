#include "Dolphin/os.h"
#include "p2_memory.h"
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>


namespace {
struct Thread {
    OSThread* key;
    OSThreadStartFunction start;
    void* argument;
    pthread_t native;
    pthread_mutex_t mutex;
    pthread_cond_t resumed;
    bool done, joined, joining, cancelRequested;
    Thread* next;
};
pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
Thread* threads;
thread_local Thread* current;
thread_local int workDepth;
// Busy game worker threads: created threads that are not blocked in a wait.
// Record/replay waits for zero between ticks (src/input_record.cpp).
std::atomic<int> busyWorkers{0};
thread_local bool barrierCounted=false;
[[noreturn]] void fail(const char* text) {
    std::fprintf(stderr,"Pikmin 2 thread service: %s\n",text); std::abort();
}
void check(int result) { if(result) fail("pthread operation failed"); }
Thread* find(OSThread* key) {
    check(pthread_mutex_lock(&registry));
    auto* t=threads;
    while(t && t->key!=key) t=t->next;
    check(pthread_mutex_unlock(&registry));
    return t;
}
void unlock(void* mutex) { check(pthread_mutex_unlock(static_cast<pthread_mutex_t*>(mutex))); }
void finish(void* record) {
    p2_critical_thread_exit();
    if(barrierCounted) { barrierCounted=false; busyWorkers.fetch_sub(1); }
    auto* t=static_cast<Thread*>(record);
    check(pthread_mutex_lock(&t->mutex));
    t->done=true;
    t->key->state=(t->key->attr&OS_THREAD_ATTR_DETACH) ? OS_THREAD_STATE_NULL : OS_THREAD_STATE_MORIBUND;
    check(pthread_cond_broadcast(&t->resumed));
    check(pthread_mutex_unlock(&t->mutex));
}
void* launch(void* record) {
    auto* t=static_cast<Thread*>(record); current=t;
    barrierCounted=true; busyWorkers.fetch_add(1);
    void* result=nullptr;
    pthread_cleanup_push(finish,t);
    p2_thread_checkpoint();
    result=t->start(t->argument);
    check(pthread_mutex_lock(&t->mutex)); t->key->val=result; check(pthread_mutex_unlock(&t->mutex));
    pthread_cleanup_pop(1);
    return result;
}
bool join(Thread* t) {
    if(pthread_equal(t->native,pthread_self())) return false;
    check(pthread_mutex_lock(&t->mutex));
    if(t->joined) { check(pthread_mutex_unlock(&t->mutex)); return true; }
    if(t->joining) { check(pthread_mutex_unlock(&t->mutex)); return false; }
    t->joining=true; check(pthread_mutex_unlock(&t->mutex));
    check(pthread_join(t->native,nullptr));
    check(pthread_mutex_lock(&t->mutex)); t->joined=true; t->joining=false;
    t->key->state=OS_THREAD_STATE_NULL;
    check(pthread_mutex_unlock(&t->mutex)); return true;
}
}

extern "C" OSThread* OSGetCurrentThread() {
    if(current) return current->key;
    static thread_local OSThread caller=[] {
        OSThread t{}; t.state=OS_THREAD_STATE_RUNNING; t.priority=t.base=16;
        t.stackBase=static_cast<u8*>(pthread_get_stackaddr_np(pthread_self()));
        t.stackEnd=reinterpret_cast<u32*>(t.stackBase-pthread_get_stacksize_np(pthread_self()));
        return t;
    }();
    return &caller;
}
extern "C" void p2_thread_checkpoint() {
    if(!current || p2_critical_section_held()) return;
    auto* t=current;
    check(pthread_mutex_lock(&t->mutex));
    pthread_cleanup_push(unlock,&t->mutex);
    if(t->key->suspend>0 && !t->cancelRequested) {
        p2_worker_idle_begin();
        while(t->key->suspend>0 && !t->cancelRequested) check(pthread_cond_wait(&t->resumed,&t->mutex));
        p2_worker_idle_end();
    }
    t->key->state=OS_THREAD_STATE_RUNNING;
    pthread_cleanup_pop(1);
    if(p2_thread_should_stop()) OSExitThread(nullptr);
}
extern "C" BOOL OSCreateThread(OSThread* key,OSThreadStartFunction start,void* argument,
    void* stack,u32 stackSize,OSPriority priority,u16 attr) {
    if(!key || !start || !stack || !stackSize || priority<0 || priority>31) return FALSE;
    if(auto* old=find(key)) {
        check(pthread_mutex_lock(&old->mutex)); const bool done=old->done; check(pthread_mutex_unlock(&old->mutex));
        if(!done) return FALSE;
        p2_destroy_thread(key);
    }
    auto* t=static_cast<Thread*>(std::calloc(1,sizeof(Thread)));
    if(!t) return FALSE;
    t->key=key; t->start=start; t->argument=argument;
    check(pthread_mutex_init(&t->mutex,nullptr)); check(pthread_cond_init(&t->resumed,nullptr));
    *key=OSThread{}; key->state=OS_THREAD_STATE_READY; key->attr=attr;
    key->suspend=1; key->priority=key->base=priority;
    key->stackBase=static_cast<u8*>(stack); key->stackEnd=reinterpret_cast<u32*>(key->stackBase-stackSize);
    // Console stack buffers are too small for native C++/libc. pthread owns a
    // separate host stack; the SDK record keeps the caller's logical bounds.
    pthread_attr_t attributes; check(pthread_attr_init(&attributes));
    check(pthread_attr_setstacksize(&attributes,std::max<size_t>(512*1024,(size_t(stackSize)+16383)&~size_t(16383))));
    const int result=pthread_create(&t->native,&attributes,launch,t);
    check(pthread_attr_destroy(&attributes));
    if(result) {
        key->state=OS_THREAD_STATE_NULL;
        check(pthread_cond_destroy(&t->resumed)); check(pthread_mutex_destroy(&t->mutex)); std::free(t); return FALSE;
    }
    check(pthread_mutex_lock(&registry)); t->next=threads; threads=t; check(pthread_mutex_unlock(&registry));
    return TRUE;
}
extern "C" s32 OSResumeThread(OSThread* key) {
    auto* t=find(key); if(!t) return 0;
    check(pthread_mutex_lock(&t->mutex)); const s32 previous=key->suspend;
    if(!t->done && key->suspend>0 && --key->suspend==0) check(pthread_cond_broadcast(&t->resumed));
    check(pthread_mutex_unlock(&t->mutex)); return previous;
}
extern "C" s32 OSSuspendThread(OSThread* key) {
    auto* t=find(key); if(!t) fail("suspending an unmanaged thread");
    check(pthread_mutex_lock(&t->mutex)); const s32 previous=key->suspend;
    if(!t->done) ++key->suspend;
    check(pthread_mutex_unlock(&t->mutex));
    // Suspension is cooperative at queue operations and yield points. Native
    // execution is not asynchronously preempted in arbitrary C++/heap code.
    if(t==current) p2_thread_checkpoint();
    return previous;
}
extern "C" BOOL OSIsThreadSuspended(OSThread* key) {
    auto* t=find(key); if(!t) return key && key->suspend>0;
    check(pthread_mutex_lock(&t->mutex)); const bool value=key->suspend>0; check(pthread_mutex_unlock(&t->mutex)); return value;
}
extern "C" BOOL OSIsThreadTerminated(OSThread* key) {
    auto* t=find(key); if(!t) return !key || key->state==OS_THREAD_STATE_NULL || key->state==OS_THREAD_STATE_MORIBUND;
    check(pthread_mutex_lock(&t->mutex)); const bool value=t->done; check(pthread_mutex_unlock(&t->mutex)); return value;
}
extern "C" void OSDetachThread(OSThread* key) {
    auto* t=find(key); if(!t) return;
    check(pthread_mutex_lock(&t->mutex)); key->attr|=OS_THREAD_ATTR_DETACH;
    if(t->done) key->state=OS_THREAD_STATE_NULL;
    check(pthread_mutex_unlock(&t->mutex));
    // Keep the pthread joinable internally until SDK-record destruction, so
    // freeing the game object can never race a detached native worker.
}
extern "C" BOOL OSJoinThread(OSThread* key,void** value) {
    auto* t=find(key); if(!t) return FALSE;
    check(pthread_mutex_lock(&t->mutex)); const bool unavailable=(key->attr&OS_THREAD_ATTR_DETACH) || t->joined; check(pthread_mutex_unlock(&t->mutex));
    if(unavailable || !join(t)) return FALSE;
    if(value) *value=key->val;
    return TRUE;
}
extern "C" void OSExitThread(void* value) {
    if(!current) fail("exiting an unmanaged thread");
    check(pthread_mutex_lock(&current->mutex)); current->key->val=value; check(pthread_mutex_unlock(&current->mutex));
    pthread_exit(value);
}
extern "C" void OSCancelThread(OSThread* key) {
    auto* t=find(key); if(!t) return;
    if(t==current) OSExitThread(nullptr);
    check(pthread_mutex_lock(&t->mutex));
    t->cancelRequested=true;
    // A protected callback may have suspended cooperatively. Let it finish
    // its cleanup before deferred cancellation, even if no caller resumes it.
    key->suspend=0; check(pthread_cond_broadcast(&t->resumed));
    check(pthread_mutex_unlock(&t->mutex));
    if(!join(t)) fail("concurrent thread destruction");
}
extern "C" void p2_destroy_thread(void* key) {
    auto* t=find(static_cast<OSThread*>(key)); if(!t) return;
    OSCancelThread(t->key);
    check(pthread_mutex_lock(&registry)); auto** link=&threads;
    while(*link!=t) link=&(*link)->next;
    *link=t->next; check(pthread_mutex_unlock(&registry));
    check(pthread_cond_destroy(&t->resumed)); check(pthread_mutex_destroy(&t->mutex)); std::free(t);
}
extern "C" void OSYieldThread() { p2_thread_checkpoint(); sched_yield(); }
extern "C" int p2_begin_thread_work() {
    return workDepth++;
}
extern "C" void p2_end_thread_work(int previous) {
    workDepth=previous;
    p2_thread_checkpoint();
}
extern "C" OSPriority OSGetThreadPriority(OSThread* key) {
    auto* t=find(key); if(!t) return key->priority;
    check(pthread_mutex_lock(&t->mutex)); const auto value=key->priority; check(pthread_mutex_unlock(&t->mutex)); return value;
}
extern "C" BOOL OSSetThreadPriority(OSThread* key,OSPriority priority) {
    if(!key || priority<0 || priority>31) return FALSE;
    auto* t=find(key); if(t) check(pthread_mutex_lock(&t->mutex));
    key->priority=key->base=priority;
    if(t) check(pthread_mutex_unlock(&t->mutex));
    return TRUE; // SDK bookkeeping; macOS schedules native workers normally.
}
extern "C" void OSInitThreadQueue(OSThreadQueue* queue) { *queue=OSThreadQueue{}; }
// Sleep/wakeup on a thread queue. Each wakeup advances the queue's generation;
// a sleeper returns once it changes. The OSThreadQueue fields stay SDK-shaped,
// so the generation lives in a small side table keyed by queue address.
namespace {
pthread_mutex_t queueLock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueWoken=PTHREAD_COND_INITIALIZER;
struct QueueGeneration { const OSThreadQueue* queue; unsigned long long generation; };
QueueGeneration generations[64];
unsigned long long& generationOf(const OSThreadQueue* queue) {
    for(auto& entry:generations) if(entry.queue==queue) return entry.generation;
    for(auto& entry:generations) if(!entry.queue) { entry.queue=queue; return entry.generation; }
    std::fputs("Pikmin 2: too many sleeping thread queues\n",stderr); std::abort();
}
}
extern "C" void OSSleepThread(OSThreadQueue* queue) {
    check(pthread_mutex_lock(&queueLock));
    const auto start=generationOf(queue);
    p2_worker_idle_begin();
    while(generationOf(queue)==start) check(pthread_cond_wait(&queueWoken,&queueLock));
    p2_worker_idle_end();
    check(pthread_mutex_unlock(&queueLock));
}
extern "C" void OSWakeupThread(OSThreadQueue* queue) {
    check(pthread_mutex_lock(&queueLock));
    ++generationOf(queue);
    check(pthread_cond_broadcast(&queueWoken));
    check(pthread_mutex_unlock(&queueLock));
}

extern "C" int p2_thread_should_stop() {
    if(!current || workDepth) return 0;
    check(pthread_mutex_lock(&current->mutex)); const bool stop=current->cancelRequested;
    check(pthread_mutex_unlock(&current->mutex)); return stop;
}

extern "C" int p2_thread_cancel_requested() {
    if(!current) return 0;
    check(pthread_mutex_lock(&current->mutex)); const bool stop=current->cancelRequested;
    check(pthread_mutex_unlock(&current->mutex)); return stop;
}

extern "C" void p2_thread_sleep_ticks(int64_t ticks) {
    // GameCube time-base ticks run at 162 MHz / 4. Bound each wait so managed
    // threads can observe cancellation and suspension without console alarms.
    constexpr int64_t ticksPerSecond=40500000;
    constexpr int64_t quantum=ticksPerSecond/50;
    p2_thread_checkpoint();
    p2_worker_idle_begin(); // a timed sleep is waiting, not working
    while(ticks>0) {
        const int64_t step=std::min(ticks,quantum);
        const int64_t nanos=(step*1000000000+ticksPerSecond-1)/ticksPerSecond;
        std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
        ticks-=step;
        p2_thread_checkpoint();
    }
    p2_worker_idle_end();
}

// Console priority: the main game thread outranked the DVD/loader threads, so a
// job it queued could not start until it blocked (frame wait or any OS wait).
// Game code relies on that ordering (state set up after the request is still
// seen by the job), so workers wait here for the main thread to yield.
namespace {
std::mutex mainYieldMutex;
std::condition_variable mainYieldCv;
int mainBlockedDepth=0;
uint32_t mainYields=0;
}
extern "C" void p2_main_block_begin() {
    if(!pthread_main_np()) return;
    { std::lock_guard<std::mutex> lock(mainYieldMutex); ++mainBlockedDepth; ++mainYields; }
    mainYieldCv.notify_all();
}
extern "C" void p2_main_block_end() {
    if(!pthread_main_np()) return;
    std::lock_guard<std::mutex> lock(mainYieldMutex); if(mainBlockedDepth) --mainBlockedDepth;
}
extern "C" void p2_wait_main_yield() {
    if(pthread_main_np()) return;
    std::unique_lock<std::mutex> lock(mainYieldMutex);
    const uint32_t start=mainYields;
    if(!mainYieldCv.wait_for(lock,std::chrono::milliseconds(100),[&]{ return mainBlockedDepth>0 || mainYields!=start; })) {
        static int reports;
        if(reports++<8) std::fprintf(stderr,"*** worker started a job after 100 ms without the main thread yielding\n");
    }
}
extern "C" void p2_worker_idle_begin() { if(barrierCounted) busyWorkers.fetch_sub(1); p2_main_block_begin(); }
extern "C" void p2_worker_idle_end() { p2_main_block_end(); if(barrierCounted) busyWorkers.fetch_add(1); }
extern "C" int p2_busy_workers() { return busyWorkers.load(); }
// For threads that run continuously in real time (the audio render loop).
extern "C" void p2_thread_exclude_from_barrier() { if(barrierCounted) { barrierCounted=false; busyWorkers.fetch_sub(1); } }
