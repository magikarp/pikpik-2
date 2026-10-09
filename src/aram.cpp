#include "JSystem/JKernel/JKRAram.h"
#include "p2_aram.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

JKRAram* JKRAram::sAramObject;
u32 JKRAram::sSZSBufferSize=0x400;
JSUList<JKRAMCommand> JKRAram::sAramCommandList;
OSMessageQueue JKRAram::sMessageQueue;
OSMessage JKRAram::sMessageBuffer[4];
JSUList<JKRAMCommand> JKRAramPiece::sAramPieceCommandList;
OSMutex JKRAramPiece::mMutex;

JKRAram* JKRAram::create(u32 audio,u32 graphics,s32,s32,s32 priority) {
    if(!sAramObject) {
        sAramObject=new (JKRHeap::sSystemHeap,0) JKRAram(audio,graphics,priority);
        if(sAramObject) sAramObject->resume();
    }
    // Native resource decompression and stream transfers run synchronously in
    // the requesting loader. No console DSP/decompression helper threads.
    return sAramObject;
}
JKRAram::JKRAram(u32 audio,u32 graphics,s32 priority):JKRThread(0x4000,16,priority) {
    const u32 base=ARInit(mStackArray,3), size=ARGetSize();
    if(!base || audio>size-base) OSPanic(__FILE__,__LINE__,"Invalid ARAM audio reservation");
    if(graphics==UINT32_MAX) graphics=size-base-audio;
    if(graphics>size-base-audio || ((audio|graphics)&31)) OSPanic(__FILE__,__LINE__,"Invalid ARAM graphics reservation");
    mAudioMemorySize=audio; mGraphMemorySize=graphics; mUserMemorySize=size-base-audio-graphics;
    mAudioMemoryPtr=ARAlloc(audio); mGraphMemoryPtr=ARAlloc(graphics);
    mUserMemoryPtr=mUserMemorySize ? ARAlloc(mUserMemorySize) : 0;
    mAramHeap=new (JKRHeap::sSystemHeap,0) JKRAramHeap(mGraphMemoryPtr,graphics);
    OSInitMutex(&JKRAramPiece::mMutex);
    OSInitMessageQueue(&sMessageQueue,sMessageBuffer,4);
}
JKRAram::~JKRAram() {
    p2_destroy_thread(mThread);
    delete mAramHeap;
    p2_destroy_message_queue(&sMessageQueue); p2_destroy_mutex(&JKRAramPiece::mMutex);
    sAramObject=nullptr; ARReset();
}
void* JKRAram::run() {
    for(;;) {
        OSMessage value; OSReceiveMessage(&sMessageQueue,&value,OS_MESSAGE_BLOCK);
        auto* message=static_cast<JKRAramCommand*>(value);
        auto* command=static_cast<JKRAMCommand*>(message->mCommand);
        const auto kind=message->mMsgType; delete message;
        if(kind!=ARAMMSG_DMA) OSPanic(__FILE__,__LINE__,"Unknown ARAM message");
        const int work=p2_begin_thread_work(); JKRAramPiece::startDMA(command); p2_end_thread_work(work);
    }
}
void JKRAram::checkOkAddress(u8*,u32,JKRAramBlock*,u32) {
    // Host copies accept unaligned main-memory buffers. Actual ranges are
    // checked by the transfer functions, not console DMA alignment assertions.
}
void JKRAram::changeGroupIdIfNeed(u8* data,int id) {
    if(id<0 || !data) return;
    auto* owner=JKRHeap::findFromRoot(data);
    if(owner && owner->getHeapType()=='EXPH') {
        auto* block=JKRExpHeap::CMemBlock::getHeapBlock(data);
        if(block) block->newGroupId(id);
    }
}
JKRAramBlock* JKRAram::mainRamToAram(u8* source,u32 destination,u32 length,JKRExpandSwitch expand,u32 capacity,JKRHeap*,int id,u32* written) {
    if(written) *written=0;
    if(!source || !sAramObject || !length) return nullptr;
    // This API gives no compressed-source extent. Its title/font callers use
    // raw copies; bounded compressed loading is handled by the DVD ripper.
    if(expand==Switch_1 && length>=4 && (!std::memcmp(source,"Yaz0",4) || !std::memcmp(source,"Yay0",4))) {
        p2_heap_report("Compressed MRAM-to-ARAM copy needs a bounded source extent\n"); return nullptr;
    }
    if(capacity) length=std::min(length,capacity);
    JKRAramBlock* block=nullptr;
    if(!destination) {
        block=getAramHeap()->alloc(length,JKRAramHeap::AM_Head);
        if(!block) return nullptr;
        block->newGroupID(decideAramGroupId(id)); destination=block->getAddress();
    }
    if(!p2_aram_write(destination,source,length)) { delete block; return nullptr; }
    if(written) *written=length;
    return block ? block : reinterpret_cast<JKRAramBlock*>(UINTPTR_MAX);
}
u8* JKRAram::aramToMainRam(u32 address,u8* destination,u32 length,JKRExpandSwitch expand,u32 capacity,JKRHeap* heap,int id,u32* written) {
    if(written) *written=0;
    P2HostScratchScope scratch;
    try {
        if(address>ARGetSize() || length>ARGetSize()-address) return nullptr;
        p2::Bytes bytes(length);
        if(!p2_aram_read(address,bytes.data(),length)) return nullptr;
        const bool compressed=expand==Switch_1 && bytes.size()>=4 &&
            (!std::memcmp(bytes.data(),"Yaz0",4) || !std::memcmp(bytes.data(),"Yay0",4));
        if(compressed) bytes=p2::decompressResource(bytes);
        size_t count=bytes.size();
        // Original raw reads use the requested source length; expansion alone
        // applies maxExpandSize as an output capacity.
        if(compressed && capacity) count=std::min<size_t>(count,capacity);
        if(!destination) destination=static_cast<u8*>(JKRHeap::alloc(std::max<size_t>(count,1),32,heap));
        if(!destination) return nullptr;
        if(count) std::memcpy(destination,bytes.data(),count);
        changeGroupIdIfNeed(destination,id);
        if(written) *written=count;
        return destination;
    } catch(const std::exception& error) { p2_heap_reportf("ARAM expansion failed: %s\n",error.what()); return nullptr; }
}
u8* JKRAram::aramToMainRam(JKRAramBlock* block,u8* destination,u32 length,u32 offset,JKRExpandSwitch expand,u32 capacity,JKRHeap* heap,int id,u32* written) {
    if(written) *written=0;
    if(!block || offset>=block->mSize) return nullptr;
    const auto remaining=block->mSize-offset;
    return aramToMainRam(block->mAddress+offset,destination,length ? std::min(length,remaining) : remaining,expand,capacity,heap,id,written);
}
int JKRDecompressFromAramToMainRam(u32 address,void* destination,u32 length,u32 capacity,u32 offset,u32* written) {
    if(written) *written=0;
    try {
        if(!destination || address>ARGetSize() || length>ARGetSize()-address) return -1;
        p2::Bytes bytes(length);
        if(!p2_aram_read(address,bytes.data(),length) || length<4 || std::memcmp(bytes.data(),"Yaz0",4)) return -1;
        bytes=p2::decompressResource(bytes);
        if(offset>bytes.size()) return -1;
        const auto count=std::min<size_t>(capacity,bytes.size()-offset);
        if(count) std::memcpy(destination,bytes.data()+offset,count);
        if(written) *written=count; return 0;
    } catch(const std::exception&) { return -1; }
}
bool JKRAramPiece::orderSync(int direction,u32 source,u32 destination,u32 length,JKRAramBlock*) {
    ARQRequest request; ARQPostRequest(&request,0,direction,0,source,destination,length,nullptr); return true;
}
void JKRAramPiece::sendCommand(JKRAMCommand* command) { startDMA(command); }
void JKRAramPiece::startDMA(JKRAMCommand* command) {
    ARQPostRequest(command,0,command->mDirection,0,command->mSource,command->mDestination,command->mLength,doneDMA);
}
void JKRAramPiece::doneDMA(uintptr_t address) {
    auto* command=reinterpret_cast<JKRAMCommand*>(address);
    if(command->_60) OSPanic(__FILE__,__LINE__,"Console chained ARAM/decompression command is not ported");
    if(command->mCallback) command->mCallback(command);
    else OSSendMessage(command->_5C ? command->_5C : &command->mMessageQueue,command,OS_MESSAGE_NOBLOCK);
}
