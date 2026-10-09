#include "JSystem/JKernel/JKRDvdAramRipper.h"
#include "JSystem/JKernel/JKRDecomp.h"
#include "p2_aram.h"
#include "p2_assets.h"
#include "p2_game_alloc.h"
#include <algorithm>
#include <cstring>
#include <vector>

JSUList<JKRADCommand> JKRDvdAramRipper::sDvdAramAsyncList;
bool JKRDvdAramRipper::errorRetry=true;
int JKRDvdAramRipper::sSZSBufferSize=0x400;

namespace {
JKRAramBlock* load(JKRDvdFile* file,u32 destination,JKRExpandSwitch expand,u32 offset,u32 limit,u32 destinationSize,u32* output) {
    if(output) *output=0;
    if(!file || !file->mFileOpen || !JKRAram::sAramObject) return nullptr;
    u8 header[4]{};
    const bool compressed=expand==Switch_1 && file->getFileSize()>=4 && file->readData(header,4,0)==4 &&
        (!std::memcmp(header,"Yaz0",4) || !std::memcmp(header,"Yay0",4));
    u32 capacity=limit;
    if(!compressed) {
        // The console's raw size is a file limit before the raw offset, rounded
        // for DMA. Its compressed size is instead a decoded-output capacity.
        u32 end=file->getFileSize(); if(limit) end=std::min(end,limit);
        if(end>UINT32_MAX-31) return nullptr;
        end=(end+31)&~u32(31); if(offset>=end) return nullptr;
        capacity=end-offset; expand=Switch_0;
    }
    // The console streams DVD data into ARAM without a main-RAM copy. Stage the
    // whole file in host memory instead of the game's (nearly full) system heap.
    if(compressed) {
        u8 yaz[8]{};
        if(file->readData(yaz,8,0)!=8) return nullptr;
        const u32 decoded=(u32(yaz[4])<<24)|(u32(yaz[5])<<16)|(u32(yaz[6])<<8)|yaz[7];
        capacity=limit ? std::min(limit,decoded) : decoded;
        if(!capacity) return nullptr;
    }
    P2HostScratchScope scratch;
    std::vector<u8> staging(capacity);
    u32 count=0;
    auto* bytes=static_cast<u8*>(JKRDvdRipper::loadToMainRAM(file,staging.data(),expand,capacity,JKRHeap::sSystemHeap,
        JKRDvdRipper::ALLOC_DIR_BOTTOM,offset,nullptr,&count));
    if(!bytes || !count || count>destinationSize) return nullptr;
    JKRAramBlock* block=nullptr;
    if(!destination) {
        block=JKRAram::getAramHeap()->alloc(count,JKRAramHeap::AM_Head);
        if(!block) return nullptr;
        destination=block->mAddress;
    }
    const bool ok=p2_aram_write(destination,bytes,count);
    if(!ok) { delete block; return nullptr; }
    if(output) *output=count;
    return block ? block : reinterpret_cast<JKRAramBlock*>(UINTPTR_MAX);
}
}
JKRADCommand::JKRADCommand():JSULink<JKRADCommand>(this),mDvdFile(nullptr),mAddress(0),mBlock(nullptr),
    mExpandSwitch(Switch_0),mCallBack(nullptr),mFileOffset(0),mSize(0),mSizePtr(nullptr),mStatus(0),mDoDeleteFile(false),mStreamCommand(nullptr) {}
JKRADCommand::~JKRADCommand() { if(mDoDeleteFile) delete mDvdFile; }
JKRADCommand* JKRDvdAramRipper::loadToAram_Async(JKRDvdFile* file,u32 address,JKRExpandSwitch expand,LoadCallback callback,u32 offset,u32 size,u32* written) {
    auto* command=new (JKRHeap::sSystemHeap,-4) JKRADCommand;
    if(!command) return nullptr;
    command->mDvdFile=file; command->mAddress=address; command->mExpandSwitch=expand;
    command->mCallBack=callback; command->mFileOffset=offset; command->mSize=size; command->mSizePtr=written;
    if(!callCommand_Async(command)) { delete command; return nullptr; }
    return command;
}
JKRADCommand* JKRDvdAramRipper::callCommand_Async(JKRADCommand* command) {
    if(!command || !command->mDvdFile) return nullptr;
    auto* file=command->mDvdFile;
    if(command->mSizePtr) *command->mSizePtr=0;
    OSLockMutex(&file->mAramMutex);
    if(file->mCommandThread) { OSUnlockMutex(&file->mAramMutex); return nullptr; }
    file->mCommandThread=OSGetCurrentThread();
    const auto destination=command->mBlock ? command->mBlock->mAddress : command->mAddress;
    auto* result=load(file,destination,command->mExpandSwitch,command->mFileOffset,command->mSize,
        command->mBlock ? command->mBlock->mSize : UINT32_MAX,command->mSizePtr);
    command->mStatus=result ? 0 : -1;
    if(result && !destination) { command->mBlock=result; command->mAddress=result->mAddress; }
    file->mCommandThread=nullptr;
    OSUnlockMutex(&file->mAramMutex);
    if(!result) return nullptr;
    // Native I/O completes inline. Callbacks receive a real native pointer,
    // outside the file lock, and may immediately query completion.
    if(command->mCallBack) command->mCallBack(reinterpret_cast<uintptr_t>(command));
    return command;
}
bool JKRDvdAramRipper::syncAram(JKRADCommand* command,BOOL) { return command && command->mStatus>=0; }
JKRAramBlock* JKRDvdAramRipper::loadToAram(JKRDvdFile* file,u32 address,JKRExpandSwitch expand,u32 offset,u32 size,u32* written) {
    auto* command=loadToAram_Async(file,address,expand,nullptr,offset,size,written);
    if(!command) return nullptr;
    auto* result=address ? reinterpret_cast<JKRAramBlock*>(UINTPTR_MAX) : command->mBlock;
    delete command; return result;
}
int JKRDecompressFromDVDToAram(JKRDvdFile* file,u32 address,u32 sourceLimit,u32 capacity,u32 decodedOffset,u32 sourceOffset,u32* written) {
    P2HostScratchScope scratch;
    if(written) *written=0;
    if(!capacity || address>ARGetSize() || capacity>ARGetSize()-address) return -1;
    p2::Bytes bytes(capacity); u32 count=0;
    if(JKRDecompressFromDVD(file,bytes.data(),sourceLimit,capacity,decodedOffset,sourceOffset,&count)<0 ||
        !p2_aram_write(address,bytes.data(),count)) return -1;
    if(written) *written=count; return 0;
}
