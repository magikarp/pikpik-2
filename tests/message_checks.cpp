#include "Dolphin/os.h"
#include "p2_memory.h"
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>
#include <chrono>

static void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
int main() {
    OSMessageQueue queue; OSMessage storage[2];
    OSInitMessageQueue(&queue,storage,2);
    int first=1, second=2, priority=3;
    OSMessage value=&priority;
    require(!OSReceiveMessage(&queue,&value,0) && value==&priority,"empty nonblocking receive preserves output");
    require(OSSendMessage(&queue,nullptr,0) && OSSendMessage(&queue,&first,0),"queue accepts null and pointer messages");
    require(!OSSendMessage(&queue,&second,0) && !OSJamMessage(&queue,&priority,0),"full nonblocking send/jam fail");
    require(OSReceiveMessage(&queue,&value,0) && !value,"null is a real message");
    require(OSJamMessage(&queue,&priority,0),"priority message inserted at head");
    require(OSReceiveMessage(&queue,&value,0) && value==&priority,"jam precedes existing message");
    require(OSReceiveMessage(&queue,&value,0) && value==&first,"original message follows jam");
    auto* mainThread=OSGetCurrentThread();
    require(mainThread && mainThread==OSGetCurrentThread() && mainThread->stackBase>reinterpret_cast<u8*>(mainThread->stackEnd),"stable native thread identity and stack extent");
    auto receive=std::async(std::launch::async,[&] {
        require(OSGetCurrentThread()!=mainThread,"native threads have distinct SDK identities");
        OSMessage result=nullptr;
        require(OSReceiveMessage(&queue,&result,OS_MESSAGE_BLOCK),"blocking receive succeeds");
        return result;
    });
    require(receive.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"receive waits for a message");
    require(OSSendMessage(&queue,&second,0) && receive.get()==&second,"send wakes blocked receiver");
    require(OSSendMessage(&queue,&first,0) && OSSendMessage(&queue,&second,0),"fill queue before blocked send");
    auto send=std::async(std::launch::async,[&] { return OSJamMessage(&queue,&priority,OS_MESSAGE_BLOCK); });
    require(send.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"sender waits for space");
    require(OSReceiveMessage(&queue,&value,0) && value==&first && send.get(),"receive wakes blocked sender");
    require(OSReceiveMessage(&queue,&value,0) && value==&priority,"blocked jam retains priority");
    require(OSReceiveMessage(&queue,nullptr,0),"receive may discard message");
    p2_destroy_message_queue(&queue);

    OSMessage ring[7]; OSInitMessageQueue(&queue,ring,7);
    constexpr unsigned producers=4, each=1000;
    constexpr uintptr_t prefix=uintptr_t(1)<<40;
    std::vector<std::thread> workers;
    for(unsigned p=0;p<producers;++p) workers.emplace_back([&,p] {
        for(unsigned n=0;n<each;++n)
            require(OSSendMessage(&queue,reinterpret_cast<void*>(prefix+p*each+n),OS_MESSAGE_BLOCK),"concurrent producer send");
    });
    bool seen[producers*each]={};
    for(unsigned n=0;n<producers*each;++n) {
        require(OSReceiveMessage(&queue,&value,OS_MESSAGE_BLOCK),"concurrent consumer receive");
        const uintptr_t id=reinterpret_cast<uintptr_t>(value)-prefix;
        require(id<producers*each && !seen[id],"messages preserve upper pointer bits without duplication");
        seen[id]=true;
    }
    for(auto& worker:workers) worker.join();
    p2_destroy_message_queue(&queue);
    for(unsigned i=0;i<64;++i) {
        OSInitMessageQueue(&queue,storage,2);
        require(OSSendMessage(&queue,&first,0) && OSReceiveMessage(&queue,&value,0) && value==&first,"queue storage can be reused after destruction");
        p2_destroy_message_queue(&queue);
    }
    std::puts("Native OS queues: blocking, priority, wraparound, cleanup and 4,000 concurrent 64-bit messages pass.");
}
