#include "JSystem/JKernel/JKRThread.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>


static void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
struct Work {
    OSThread record{};
    alignas(32) u8 stack[4096];
    std::atomic<int> stage{0};
};
static void* run(void* arg) {
    auto* w=static_cast<Work*>(arg);
    require(OSGetCurrentThread()==&w->record,"native worker uses the requested SDK identity");
    w->stage=1;
    OSSuspendThread(OSGetCurrentThread());
    w->stage=2;
    return w;
}
static void reached(Work& work,int value) {
    for(int i=0; i<1000 && work.stage!=value; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(work.stage==value,"worker reached expected stage");
}
struct WaitingThread : JKRThread {
    std::atomic<bool> started{false};
    WaitingThread():JKRThread(u32(4096),8,16) {}
    void* run() override { started=true; return waitMessageBlock(); }
};
static void taskCallback(void* value) { ++*static_cast<std::atomic<int>*>(value); }
struct ActiveWork { std::atomic<bool> entered{false},proceed{false},finished{false}; };
static void activeCallback(void* value) {
    auto* work=static_cast<ActiveWork*>(value); work->entered=true;
    while(!work->proceed) OSYieldThread();
    work->finished=true;
}
struct AssetWork { std::atomic<bool> finished{false}; };
static void loadTitle(void* value) {
    auto* work=static_cast<AssetWork*>(value);
    u32 size=0;
    void* bytes=JKRDvdRipper::loadToMainRAM("user/Ebisawa/title/title.szs",nullptr,Switch_1,0,
        nullptr,JKRDvdRipper::ALLOC_DIR_TOP,0,nullptr,&size);
    require(bytes && size>64 && !std::memcmp(bytes,"RARC",4),"original loading task reads and expands actual title archive");
    JKRHeap::free(bytes,nullptr); work->finished=true;
}
static void* explicitExit(void* arg) { OSExitThread(arg); return nullptr; }
int main(int argc,const char** argv) {
    require(p2_memory_init(128*1024*1024),"native arena");
    auto* heap=JKRExpHeap::createRoot(16,false); require(heap,"game heap");
    for(int round=0;round<20;++round) {
        Work work;
        require(OSCreateThread(&work.record,run,&work,work.stack+sizeof(work.stack),sizeof(work.stack),16,0),"native create");
        require(OSIsThreadSuspended(&work.record) && work.stage==0,"created thread waits for resume");
        require(OSSuspendThread(&work.record)==1 && OSResumeThread(&work.record)==2,"nested suspension count");
        require(work.stage==0 && OSResumeThread(&work.record)==1,"final resume starts execution");
        reached(work,1);
        // The worker signals before entering its self-suspension. Wait for the
        // SDK suspend count, avoiding a timing assumption about host scheduling.
        for(int i=0;i<1000 && !OSIsThreadSuspended(&work.record);++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(OSIsThreadSuspended(&work.record),"self suspension");
        require(OSSetThreadPriority(&work.record,7) && OSGetThreadPriority(&work.record)==7,"SDK priority metadata");
        OSResumeThread(&work.record);
        void* value=nullptr; require(OSJoinThread(&work.record,&value) && value==&work && work.stage==2,"join returns full-width value");
        require(OSIsThreadTerminated(&work.record),"joined thread terminated");
        p2_destroy_thread(&work.record);
        require(OSCreateThread(&work.record,explicitExit,&work,work.stack+sizeof(work.stack),sizeof(work.stack),16,0),"reused record");
        OSResumeThread(&work.record);
        require(OSJoinThread(&work.record,&value) && value==&work,"explicit exit runs cleanup and preserves result");
        p2_destroy_thread(&work.record);
    }
    u32 baseline=0;
    for(int round=0;round<20;++round) {
        auto* worker=new WaitingThread;
        require(heap->do_getSize(worker->mMsgBuffer)>=s32(8*sizeof(OSMessage)),"original JKR queue allocates native pointer width");
        worker->resume();
        while(!worker->started) std::this_thread::yield();
        // Deleting a worker while it is waiting must join its native thread and
        // release the queue lock/waiter count before destroying queue storage.
        delete worker;
        if(!round) baseline=heap->getTotalFreeSize();
        else require(heap->getTotalFreeSize()==baseline,"JKR thread lifetimes release heap storage");
    }
    for(int round=0;round<10;++round) {
        std::atomic<int> calls{0};
        auto* task=JKRTask::create(8,16,4096,heap);
        require(task,"original task creation");
        std::thread producer([&] { for(int i=0;i<100;++i) while(!task->request(taskCallback,&calls,nullptr)) std::this_thread::yield(); });
        for(int i=0;i<100;++i) while(!task->request(taskCallback,&calls,nullptr)) std::this_thread::yield();
        producer.join();
        while(calls!=200) std::this_thread::yield();
        delete task;
        require(heap->getTotalFreeSize()==baseline,"original task messages, worker, queues and heap storage released");
    }
    {
        ActiveWork work;
        auto* task=JKRTask::create(1,16,4096,heap);
        require(task && task->request(activeCallback,&work,nullptr),"active callback queued");
        while(!work.entered) std::this_thread::yield();
        OSSuspendThread(task->getThreadRecord());
        require(OSIsThreadSuspended(task->getThreadRecord()),"active callback can suspend cooperatively");
        std::atomic<bool> destroying{false},destroyed{false};
        std::thread destroyer([&] { destroying=true; delete task; destroyed=true; });
        while(!destroying) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(!destroyed && !work.finished,"task destruction waits for its active callback");
        work.proceed=true; destroyer.join();
        require(destroyed && work.finished && heap->getTotalFreeSize()==baseline,"callback completes before task storage is freed");
    }
    if(argc==2) {
        require(p2_dvd_mount(argv[1]),"mount real disc for task loading");
        AssetWork work;
        auto* task=JKRTask::create(1,16,4096,heap);
        require(task && task->request(loadTitle,&work,nullptr),"title loading task queued");
        while(!work.finished) std::this_thread::yield();
        delete task;
        require(p2_dvd_mount(argv[1]),"loading task released DVD handle");
    }
    { JKRThread mainThread(OSGetCurrentThread(),1); require(mainThread.mStackSize>0,"main-thread stack bounds use correct pointer order"); }
    require(heap->check(),"heap integrity after worker teardown");
    std::puts("Native thread identity/resume/suspend/join/exit and original JKR loading tasks pass; 2,000 callbacks and cancellation cleanup checked.");
}
