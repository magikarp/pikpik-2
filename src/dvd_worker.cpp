#include "DvdThreadCommand.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_memory.h"
#include <cstring>
#include <atomic>
#include <vector>

// Commands sent and not yet completed. Record/replay waits for zero each tick
// (p2_dvd_wait_idle) so a load finishes on the same tick in every run.
static std::atomic<int> pendingCommands{0};
extern "C" int p2_dvd_pending() { return pendingCommands.load(); }
DvdThreadCommand::~DvdThreadCommand() {
    // Direct mode polling is used by game states. Completion can be visible
    // just before the worker releases this mutex, so wait before destroying it.
    for(;;) {
        OSLockMutex(&mMutex); const bool queued=mNativeQueued; OSUnlockMutex(&mMutex);
        if(!queued) break;
        OSMessage message; OSReceiveMessage(&mMsgQueue,&message,OS_MESSAGE_BLOCK);
    }
    p2_destroy_message_queue(&mMsgQueue); p2_destroy_mutex(&mMutex);
}
DvdThread::~DvdThread() {
    syncAll(BLOCKFLAG_Unk0);
    p2_destroy_thread(mThread);
    p2_destroy_mutex(&mNativeListMutex);
}
bool DvdThreadCommand::checkExp(const char* extension) const {
    if(!mArcPath || !extension) return false;
    const char* dot=std::strrchr(mArcPath,'.');
    return dot && !std::strcmp(dot+1,extension);
}
void* DvdThread::run() {
    JKRHeap::sCurrentHeap=nullptr; // No section heap is retained while idle.
    for(;;) {
        OSMessage message;
        OSReceiveMessage(&mMsgQueue,&message,OS_MESSAGE_BLOCK);
        auto* cmd=static_cast<DvdThreadCommand*>(message);
        p2_wait_main_yield(); // the submitter finishes its frame's setup first, as on console
        const int work=p2_begin_thread_work();
        OSLockMutex(&cmd->mMutex);
        cmd->mMode=DvdThreadCommand::CM_Processing;
        JKRHeap::sCurrentHeap=cmd->mNativeSubmissionHeap;
        switch(cmd->mLoadType) {
        case DvdThreadCommand::LT_Callback: cmd->invokeCallBack(); break;
        case DvdThreadCommand::LT_Archive: loadArchive(cmd); break;
        case DvdThreadCommand::LT_File: loadFile(cmd); break;
        default: OSPanic(__FILE__,__LINE__,"Unknown DVD command");
        }
        // Console threads shared one current-heap global, so a heap a callback
        // selects (setupFloatMemory's float heap) is current for the submitter
        // too; VS/Challenge postSetupFloatMemory allocates into it on the main
        // thread. Hand it back if the submitter has not changed heap since.
        if(JKRHeap::sCurrentHeap!=cmd->mNativeSubmissionHeap && cmd->mNativeSubmitterHeap &&
           *cmd->mNativeSubmitterHeap==cmd->mNativeSubmissionHeap)
            *cmd->mNativeSubmitterHeap=JKRHeap::sCurrentHeap;
        // Drop the worker's own selection before the owner observes completion
        // and is allowed to destroy that section.
        JKRHeap::sCurrentHeap=nullptr;
        OSLockMutex(&mNativeListMutex);
        mCommandList.remove(&cmd->mLink);
        cmd->mNativeQueued=false;
        cmd->mMode=DvdThreadCommand::CM_Completed;
        OSUnlockMutex(&mNativeListMutex);
        pendingCommands.fetch_sub(1);
        OSSendMessage(&cmd->mMsgQueue,reinterpret_cast<void*>(uintptr_t(0x44544c46)),OS_MESSAGE_NOBLOCK);
        OSUnlockMutex(&cmd->mMutex);
        // No command access after unlocking: a synchronized owner can free it.
        p2_end_thread_work(work);
    }
}
void DvdThread::loadFile(DvdThreadCommand* cmd) {
    JKRDvdFile file(cmd->getArcPath());
    if(!file.mFileOpen) OSPanic(__FILE__,__LINE__,"Cannot open DVD file");
    // Explicit heap selection avoids changing another worker's allocation heap.
    void* bytes=JKRDvdRipper::loadToMainRAM(&file,nullptr,Switch_0,file.getFileSize(),cmd->mHeap,
        cmd->mHeapDirection==DvdThreadCommand::EHD_Unknown1 ? JKRDvdRipper::ALLOC_DIR_TOP : JKRDvdRipper::ALLOC_DIR_BOTTOM,
        0,nullptr,nullptr);
    if(!bytes) OSPanic(__FILE__,__LINE__,"Cannot read DVD file");
    cmd->mMountedArchive=static_cast<JKRArchive*>(bytes);
}
void DvdThread::sendCommand(DvdThreadCommand* cmd) {
    OSLockMutex(&cmd->mMutex);
    if(cmd->mNativeQueued) OSPanic(__FILE__,__LINE__,"DVD command already queued");
    OSMessage old;
    while(OSReceiveMessage(&cmd->mMsgQueue,&old,OS_MESSAGE_NOBLOCK)) {}
    cmd->mNativeSubmissionHeap=JKRHeap::sCurrentHeap;
    cmd->mNativeSubmitterHeap=&JKRHeap::sCurrentHeap; // this thread's slot
    cmd->mMode=DvdThreadCommand::CM_Initialized;
    cmd->mNativeQueued=true;
    OSLockMutex(&mNativeListMutex);
    if(!mCommandList.append(&cmd->mLink)) OSPanic(__FILE__,__LINE__,"DVD command already queued");
    OSUnlockMutex(&mNativeListMutex);
    OSUnlockMutex(&cmd->mMutex);
    pendingCommands.fetch_add(1);
    OSSendMessage(&mMsgQueue,cmd,OS_MESSAGE_BLOCK);
}
bool DvdThread::sync(DvdThreadCommand* cmd,ESyncBlockFlag flag) {
    for(;;) {
        if(flag==BLOCKFLAG_Unk0) OSLockMutex(&cmd->mMutex);
        else if(!OSTryLockMutex(&cmd->mMutex)) return false;
        const bool complete=cmd->mMode==DvdThreadCommand::CM_Completed;
        OSUnlockMutex(&cmd->mMutex);
        if(complete || flag!=BLOCKFLAG_Unk0) return complete;
        OSMessage message;
        OSReceiveMessage(&cmd->mMsgQueue,&message,OS_MESSAGE_BLOCK);
        if(reinterpret_cast<uintptr_t>(message)!=0x44544c46) OSPanic(__FILE__,__LINE__,"Invalid DVD completion");
    }
}
int DvdThread::syncAll(ESyncBlockFlag flag) {
    std::vector<DvdThreadCommand*> commands;
    OSLockMutex(&mNativeListMutex);
    for(auto* link=mCommandList.getFirst();link;link=link->getNext()) commands.push_back(link->getObject());
    OSUnlockMutex(&mNativeListMutex);
    int pending=0;
    for(auto* cmd:commands) if(!sync(cmd,flag)) ++pending;
    return pending;
}
