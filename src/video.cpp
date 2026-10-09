#include "Dolphin/vi.h"
#include "p2_video.h"
#include "p2_memory.h"
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
namespace {
std::mutex stateMutex;
std::condition_variable retraced;
std::thread::id renderThread;
VIRetraceCallback preCallback,postCallback;
u32 retraces,completedRetraces;
void* nextBuffer;
void* flushedBuffer;
void* currentBuffer;
bool nextBlack=true,flushedBlack=true,currentBlack=true,flushed;
using Clock=std::chrono::steady_clock;
Clock::time_point deadline;
}
extern "C" VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if(renderThread==std::thread::id{}) renderThread=std::this_thread::get_id();
    const auto previous=preCallback;preCallback=callback;return previous;
}
extern "C" VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    std::lock_guard<std::mutex> lock(stateMutex);
    const auto previous=postCallback;postCallback=callback;return previous;
}
extern "C" u32 VIGetRetraceCount() { std::lock_guard<std::mutex> lock(stateMutex);return retraces; }
extern "C" void VISetNextFrameBuffer(void* buffer) { std::lock_guard<std::mutex> lock(stateMutex);nextBuffer=buffer; }
extern "C" void* VIGetNextFrameBuffer() { std::lock_guard<std::mutex> lock(stateMutex);return nextBuffer; }
extern "C" void* VIGetCurrentFrameBuffer() { std::lock_guard<std::mutex> lock(stateMutex);return currentBuffer; }
extern "C" void VISetBlack(BOOL black) { std::lock_guard<std::mutex> lock(stateMutex);nextBlack=black!=0; }
extern "C" int p2_video_is_black() { std::lock_guard<std::mutex> lock(stateMutex);return currentBlack; }
extern "C" u32 VIGetDTVStatus() { return 1; } // Native display supports progressive output.
extern "C" void VIFlush() {
    std::lock_guard<std::mutex> lock(stateMutex);
    flushedBuffer=nextBuffer;flushedBlack=nextBlack;flushed=true;
}
extern "C" void p2_video_retrace() {
    VIRetraceCallback before,after;u32 count;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(renderThread==std::thread::id{}) renderThread=std::this_thread::get_id();
        if(renderThread!=std::this_thread::get_id()) {
            std::fputs("Pikmin 2: retrace callbacks require the render thread\n",stderr);std::abort();
        }
        count=++retraces;before=preCallback;
    }
    if(before) before(count);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(flushed) { currentBuffer=flushedBuffer;currentBlack=flushedBlack;flushed=false; }
        after=postCallback;
    }
    if(after) after(count);
    { std::lock_guard<std::mutex> lock(stateMutex);completedRetraces=count; }
    retraced.notify_all();
}
// P2_PERF: time the render thread spends sleeping to the 60 Hz retrace clock.
std::atomic<long long> p2_retrace_sleep_ns{0};
extern "C" void VIWaitForRetrace() {
    {
        std::unique_lock<std::mutex> lock(stateMutex);
        if(renderThread==std::thread::id{}) renderThread=std::this_thread::get_id();
        if(renderThread!=std::this_thread::get_id()) {
            const u32 before=completedRetraces;
            p2_main_block_begin();
            struct Unblock { ~Unblock() { p2_main_block_end(); } } unblock;
            while(completedRetraces==before) {
                retraced.wait_for(lock,std::chrono::milliseconds(50));
                if(p2_thread_should_stop()) { lock.unlock();p2_thread_checkpoint();return; }
            }
            return;
        }
    }
    // The retail US display is approximately 60 Hz. Keep callback execution on
    // the render thread; queued callbacks are never run by an asynchronous timer.
    // On console the retrace interrupt keeps counting while the game works, so
    // boundaries that passed since the last call fire first (late, as their
    // queued messages would be read), then the call waits for the next one.
    // Without this a frame of work plus a 2-retrace wait took three periods.
    const auto period=std::chrono::nanoseconds(16683333);
    // P2_BENCH: retraces fire without waiting, so the game runs as fast as it renders.
    static const bool uncapped=std::getenv("P2_BENCH")!=nullptr;
    if(uncapped) { p2_video_retrace(); return; }
    const auto now=Clock::now();
    if(deadline.time_since_epoch().count()==0 || now>deadline+4*period) deadline=now+period;
    while(deadline<=now) { p2_video_retrace();deadline+=period; }
    const auto sleepStart=Clock::now();
    p2_main_block_begin();
    std::this_thread::sleep_until(deadline);
    p2_main_block_end();
    p2_retrace_sleep_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-sleepStart).count());
    p2_video_retrace();
    deadline+=period;
}
