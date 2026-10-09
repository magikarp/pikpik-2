#include "Dolphin/vi.h"
#include "Dolphin/os.h"
#include "p2_video.h"
#include "p2_memory.h"
#include <cstdio>
#include <cstdint>
#include <thread>
#include <atomic>
#include <chrono>
static int failures,phase;
static std::thread::id renderThread;
static OSMessageQueue queue;
static void* selected=reinterpret_cast<void*>(uintptr_t(0x123456780000));
static void check(bool ok,const char* text) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",text);++failures; } }
static void pre(u32 count) {
    check(std::this_thread::get_id()==renderThread,"callbacks stay on render thread");
    check(phase==0&&count==VIGetRetraceCount(),"pre callback ordering and count");phase=1;
    VISetNextFrameBuffer(selected);VISetBlack(FALSE);VIFlush();
}
static void post(u32 count) {
    check(phase==1,"post callback follows pre");phase=2;
    check(VIGetCurrentFrameBuffer()==selected&&!p2_video_is_black(),"pre-flushed state is latched before post");
    OSSendMessage(&queue,reinterpret_cast<void*>(uintptr_t(count)),OS_MESSAGE_NOBLOCK);
}
int main() {
    renderThread=std::this_thread::get_id();
    OSMessage storage[1];OSInitMessageQueue(&queue,storage,1);
    check(p2_video_is_black()&&VIGetCurrentFrameBuffer()==nullptr,"initial blank display");
    VISetNextFrameBuffer(selected);VISetBlack(FALSE);
    p2_video_retrace();check(p2_video_is_black()&&VIGetCurrentFrameBuffer()==nullptr,"unflushed state is not visible");
    check(VISetPreRetraceCallback(pre)==nullptr&&VISetPostRetraceCallback(post)==nullptr,"initial callback registration");
    for(int i=0;i<5;++i) {
        phase=0;VIWaitForRetrace();OSMessage msg=nullptr;
        check(OSReceiveMessage(&queue,&msg,OS_MESSAGE_NOBLOCK)&&uintptr_t(msg)==VIGetRetraceCount(),"retrace drives real native message queue");
        check(phase==2,"callbacks complete on calling thread");
    }
    check(VISetPreRetraceCallback(nullptr)==pre&&VISetPostRetraceCallback(nullptr)==post,"callback replacement returns predecessor");
    VISetBlack(TRUE);VIFlush();VISetBlack(FALSE);p2_video_retrace();
    check(p2_video_is_black(),"flush snapshots black state independently of later writes");
    VIFlush();p2_video_retrace();check(!p2_video_is_black(),"next flush releases black display");
    std::atomic<bool> workerStarted{false},workerDone{false};
    const auto before=VIGetRetraceCount();
    std::thread worker([&] { workerStarted=true;VIWaitForRetrace();workerDone=true; });
    while(!workerStarted) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    check(!workerDone&&VIGetRetraceCount()==before,"worker wait does not drive renderer callbacks");
    while(!workerDone) { p2_video_retrace();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    worker.join();check(workerDone,"render-thread retrace wakes worker");
    check(VIGetDTVStatus()==1,"progressive native display");
    p2_destroy_message_queue(&queue);
    std::printf("Native retrace checks: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
