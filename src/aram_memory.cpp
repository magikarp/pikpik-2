#include "Dolphin/ar.h"
#include "Dolphin/os.h"
#include "p2_aram.h"
#include "p2_memory.h"
#include <pthread.h>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u32 Capacity=16*1024*1024, Base=0x4000;
u8* storage;
u32 top=Base,*sizes,count,used;
ARCallback dmaCallback;
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
struct Lock { Lock(){pthread_mutex_lock(&mutex);} ~Lock(){pthread_mutex_unlock(&mutex);} };
bool range(u32 address,size_t length) { return storage && address<=Capacity && length<=Capacity-address; }
void dma(u32 direction,u32 mainAddress,u32 aramAddress,u32 length) {
    if(u64(mainAddress)+length>UINT32_MAX) OSPanic(__FILE__,__LINE__,"MEM1 transfer overflow");
    auto* memory=p2_physical_to_host(mainAddress);
    p2_physical_to_host(mainAddress+length);
    const bool ok=direction==ARAM_DIR_MRAM_TO_ARAM ? p2_aram_write(aramAddress,memory,length)
        : direction==ARAM_DIR_ARAM_TO_MRAM && p2_aram_read(aramAddress,memory,length);
    if(!ok) OSPanic(__FILE__,__LINE__,"ARAM transfer outside backing memory");
}
}
bool p2_aram_read(u32 address,void* destination,size_t size) {
    Lock lock; if(!range(address,size) || (!destination && size)) return false;
    if(size) std::memcpy(destination,storage+address,size); return true;
}
bool p2_aram_write(u32 address,const void* source,size_t size) {
    Lock lock; if(!range(address,size) || (!source && size)) return false;
    if(size) std::memcpy(storage+address,source,size); return true;
}
const unsigned char* p2_aram_host(u32 address,size_t size) {
    return range(address,size) ? storage+address : nullptr;
}
extern "C" u32 ARInit(u32* stack,u32 entries) {
    Lock lock;
    if(sizes) return Base;
    if(!stack || !entries) return 0;
    if(!storage) storage=static_cast<u8*>(std::calloc(Capacity,1));
    if(!storage) return 0;
    sizes=stack; count=entries; used=0; top=Base;
    return Base;
}
extern "C" u32 ARAlloc(u32 length) {
    Lock lock;
    if(!sizes || used==count || length>Capacity-top || (length&31)) return 0;
    const auto address=top; top+=length; sizes[used++]=length; return address;
}
extern "C" u32 ARFree(u32* length) {
    Lock lock; if(!used) return 0;
    const auto size=sizes[--used]; top-=size; if(length) *length=size; return top;
}
extern "C" void ARReset() {
    Lock lock; sizes=nullptr; count=used=0; top=Base; dmaCallback=nullptr;
    if(storage) std::memset(storage,0,Capacity);
}
extern "C" BOOL ARCheckInit() { Lock lock; return sizes!=nullptr; }
extern "C" u32 ARGetSize() { return Capacity; }
extern "C" u32 ARGetInternalSize() { return Capacity; }
extern "C" u32 ARGetBaseAddress() { return Base; }
extern "C" u32 ARGetDMAStatus() { return 0; } // Transfers finish before returning.
extern "C" ARCallback ARRegisterDMACallback(ARCallback callback) {
    Lock lock; auto previous=dmaCallback; dmaCallback=callback; return previous;
}
extern "C" void ARStartDMA(u32 direction,u32 mainAddress,u32 aramAddress,u32 length) {
    dma(direction,mainAddress,aramAddress,length);
    ARCallback callback; { Lock lock; callback=dmaCallback; }
    if(callback) callback();
}
extern "C" void ARQInit() {} // Native transfers need no hardware request scheduler.
extern "C" void ARQPostRequest(ARQRequest* request,u32 owner,u32 direction,u32 priority,u32 source,u32 dest,u32 length,ARQCallback callback) {
    *request=ARQRequest{}; request->owner=owner; request->type=direction; request->priority=priority;
    request->source=source; request->dest=dest; request->length=length; request->callback=callback;
    dma(direction,direction==ARAM_DIR_MRAM_TO_ARAM ? source : dest,direction==ARAM_DIR_MRAM_TO_ARAM ? dest : source,length);
    if(callback) callback(reinterpret_cast<uintptr_t>(request));
}
