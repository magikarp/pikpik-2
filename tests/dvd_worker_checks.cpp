#include "DvdThreadCommand.h"
#include "IDelegate.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_assets.h"
#include "p2_dvd.h"
#include "p2_memory.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool value,const char* text) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); }
}
struct HeapCallback : IDelegate {
    JKRHeap* target;
    JKRHeap* inherited;
    std::atomic<bool> entered{false},release{false};
    void* allocation=nullptr;
    void invoke() override {
        require(JKRHeap::sCurrentHeap==inherited,"DVD worker inherited the creator's current heap");
        auto* previous=target->becomeCurrentHeap();
        allocation=new u8[96];
        entered=true;
        while(!release) OSYieldThread();
        previous->becomeCurrentHeap();
    }
};
struct SectionHeapCallback : IDelegate {
    JKRHeap* expected;
    JKRHeap* leaveSelected;
    void invoke() override {
        require(JKRHeap::sCurrentHeap==expected,"DVD callback uses the submitting section heap");
        auto* allocation=new u8[128];
        require(JKRHeap::findFromRoot(allocation)==expected,"implicit callback allocation belongs to submitting section");
        delete[] allocation;
        if(leaveSelected) leaveSelected->becomeCurrentHeap();
    }
};
struct CountCallback : IDelegate {
    std::atomic<int> count{0};
    void invoke() override { ++count; }
};
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"native arena and disc");
    auto* root=JKRExpHeap::createRoot(16,false);
    require(root && p2_dvd_mount(argv[1]),"root heap and DVD");
    auto* assetHeap=JKRExpHeap::create(32*1024*1024,root,false);
    require(assetHeap,"dedicated resource heap"); root->becomeCurrentHeap();
    const char* path="user/Ebisawa/title/title.szs";
    const auto bytes=p2::readAsset(std::filesystem::path(argv[1])/"files",path);
    DVDFileInfo warmup{}; require(DVDOpen(const_cast<char*>(path),&warmup),"warm up DVD handles"); DVDClose(&warmup);
    u32 baseline=0,assetBaseline=0;
    for(unsigned round=0;round<8;++round) {
        {
            DvdThread worker(0x8000,2,29);
            HeapCallback heapCallback; heapCallback.target=assetHeap; heapCallback.inherited=root;
            DvdThreadCommand command;
            command.loadUseCallBack(&heapCallback); worker.sendCommand(&command);
            while(!heapCallback.entered) std::this_thread::yield();
            require(!worker.sync(&command,static_cast<DvdThread::ESyncBlockFlag>(1)),"poll does not block on active callback");
            require(JKRHeap::sCurrentHeap==root,"worker heap selection is isolated from main thread");
            auto* mainAllocation=new u8[96];
            require(JKRHeap::findFromRoot(mainAllocation)==root && JKRHeap::findFromRoot(heapCallback.allocation)==assetHeap,
                "simultaneous implicit allocations use each thread's chosen heap");
            heapCallback.release=true;
            require(worker.sync(&command,DvdThread::BLOCKFLAG_Unk0),"callback completion handshake");
            delete[] mainAllocation; delete[] static_cast<u8*>(heapCallback.allocation);
            for(auto direction:{DvdThreadCommand::EHD_Unknown1,DvdThreadCommand::EHD_Unknown0}) {
                // Reuse the same command after a completion was observed without
                // necessarily consuming its notification. sendCommand drains it.
                command.mLoadType=DvdThreadCommand::LT_File;
                command.mArcPath=const_cast<char*>(path); command.mHeap=assetHeap;
                command.mHeapDirection=direction;
                worker.sendCommand(&command); require(worker.sync(&command,DvdThread::BLOCKFLAG_Unk0),"raw file command completion");
                require(command.mMountedArchive && !std::memcmp(command.mMountedArchive,bytes.data(),bytes.size()),"DVD file command returns real archive bytes");
                require(JKRHeap::findFromRoot(command.mMountedArchive)==assetHeap,"raw resource belongs to requested heap");
                JKRHeap::free(command.mMountedArchive,nullptr);
                command.mLoadType=DvdThreadCommand::LT_Archive;
                worker.sendCommand(&command); worker.syncAll(DvdThread::BLOCKFLAG_Unk0);
                require(command.mMode==DvdThreadCommand::CM_Completed && command.mMountedArchive && command.mMountedArchive->countFile(),
                    "archive command mounts real title resource tree");
                require(JKRHeap::findFromRoot(command.mMountedArchive)==assetHeap,"mounted archive belongs to requested heap");
                command.mMountedArchive->unmount(); command.mMountedArchive=nullptr;
            }
            // Reproduce a boot callback leaving a section heap selected, then
            // destroying that heap before the next title callback is submitted.
            auto* bootHeap=JKRExpHeap::create(65536,root,false);
            require(bootHeap,"temporary boot heap");
            SectionHeapCallback sectionCallback;
            sectionCallback.expected=assetHeap; sectionCallback.leaveSelected=bootHeap;
            DvdThreadCommand sectionCommand; sectionCommand.loadUseCallBack(&sectionCallback);
            assetHeap->becomeCurrentHeap(); worker.sendCommand(&sectionCommand); root->becomeCurrentHeap();
            require(worker.sync(&sectionCommand,DvdThread::BLOCKFLAG_Unk0),"section callback completed");
            bootHeap->destroy();
            sectionCallback.expected=root; sectionCallback.leaveSelected=nullptr;
            worker.sendCommand(&sectionCommand);
            require(worker.sync(&sectionCommand,DvdThread::BLOCKFLAG_Unk0),"next callback survives previous section destruction");
            CountCallback callback;
            DvdThreadCommand commands[16];
            for(auto& item:commands) item.loadUseCallBack(&callback);
            std::thread producer([&] { for(unsigned i=0;i<8;++i) worker.sendCommand(&commands[i]); });
            for(unsigned i=8;i<16;++i) worker.sendCommand(&commands[i]);
            producer.join();
            require(worker.syncAll(DvdThread::BLOCKFLAG_Unk0)==0 && callback.count==16,"concurrent producers and bounded command queue");
            for(auto& item:commands) require(item.mMode==DvdThreadCommand::CM_Completed,"all queued commands publish completion");
            {
                HeapCallback blocker; blocker.target=assetHeap; blocker.inherited=root;
                DvdThreadCommand blocking; blocking.loadUseCallBack(&blocker);
                worker.sendCommand(&blocking);
                while(!blocker.entered) std::this_thread::yield();
                CountCallback queuedCallback;
                auto* queued=new DvdThreadCommand; queued->loadUseCallBack(&queuedCallback);
                worker.sendCommand(queued);
                std::atomic<bool> destroying{false},destroyed{false};
                std::thread destroyer([&] { destroying=true; delete queued; destroyed=true; });
                while(!destroying) std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                require(!destroyed,"queued command stays alive until the worker completes it");
                blocker.release=true; destroyer.join();
                worker.sync(&blocking,DvdThread::BLOCKFLAG_Unk0);
                require(destroyed && queuedCallback.count==1,"queued command destruction waits for completion");
                delete[] static_cast<u8*>(blocker.allocation);
            }
            command.mArcPath=const_cast<char*>("extensionless"); require(!command.checkExp("arc"),"extension check cannot underflow");
        }
        require(root->check() && assetHeap->check(),"heap integrity after worker teardown");
        if(!round) { baseline=root->getTotalFreeSize(); assetBaseline=assetHeap->getTotalFreeSize(); }
        else require(root->getTotalFreeSize()==baseline && assetHeap->getTotalFreeSize()==assetBaseline,
            "worker commands and loaded resources release both heaps' storage");
        require(JKRHeap::sCurrentHeap==root,"main heap selection remains unchanged");
    }
    require(p2_dvd_mount(argv[1]),"DVD command worker released all file handles");
    assetHeap->destroy();
    std::puts("Game DvdThread: real file/archive commands, callbacks, heap isolation, polling, reuse, concurrent producers and teardown pass.");
}
