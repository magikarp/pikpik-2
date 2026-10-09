#include "Dolphin/os.h"
#include "p2_memory.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <time.h>

namespace {
struct Queue {
    OSMessageQueue* key;
    pthread_mutex_t mutex;
    pthread_cond_t readable, writable;
    unsigned waiters, readers;
    Queue* next;
};
pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
Queue* queues;
[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr,"Pikmin 2 message service: %s\n",what); std::abort();
}
void check(int result) { if(result) fail("pthread operation failed"); }
Queue* find(OSMessageQueue* key) {
    check(pthread_mutex_lock(&registry));
    auto* q=queues;
    while(q && q->key!=key) q=q->next;
    check(pthread_mutex_unlock(&registry));
    if(!q) fail("uninitialized queue");
    return q;
}
void cancelledWait(void* record) {
    auto* q=static_cast<Queue*>(record); --q->waiters;
    check(pthread_mutex_unlock(&q->mutex));
}
void wait(Queue* q,pthread_cond_t* condition) {
    ++q->waiters;
    pthread_cleanup_push(cancelledWait,q);
    timespec deadline{}; clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_nsec+=50000000;
    if(deadline.tv_nsec>=1000000000) { ++deadline.tv_sec; deadline.tv_nsec-=1000000000; }
    p2_worker_idle_begin();
    const int result=pthread_cond_timedwait(condition,&q->mutex,&deadline);
    p2_worker_idle_end();
    if(result!=ETIMEDOUT) check(result);
    pthread_cleanup_pop(0);
    --q->waiters;
    // Cooperative SDK cancellation exits outside queue locks. A bounded wait
    // observes stop requests even when no producer will send another message.
    if(p2_thread_should_stop()) {
        check(pthread_mutex_unlock(&q->mutex));
        p2_thread_checkpoint();
        fail("cancelled thread unexpectedly resumed");
    }
}
BOOL send(OSMessageQueue* queue,OSMessage message,s32 flags,bool jam) {
    p2_thread_checkpoint();
    auto* q=find(queue); check(pthread_mutex_lock(&q->mutex));
    while(queue->usedCount>=queue->msgCount) {
        if(!(flags&OS_MESSAGE_BLOCK)) { check(pthread_mutex_unlock(&q->mutex)); return FALSE; }
        wait(q,&q->writable);
    }
    s32 index;
    if(jam) {
        queue->firstIndex=queue->firstIndex ? queue->firstIndex-1 : queue->msgCount-1;
        index=queue->firstIndex;
    } else index=(s64(queue->firstIndex)+queue->usedCount)%queue->msgCount;
    queue->msgArray[index]=message; ++queue->usedCount;
    check(pthread_cond_signal(&q->readable)); check(pthread_mutex_unlock(&q->mutex));
    p2_thread_checkpoint();
    return TRUE;
}
}
extern "C" void OSInitMessageQueue(OSMessageQueue* queue,OSMessage* storage,s32 capacity) {
    if(!queue || capacity<0 || (capacity && !storage)) fail("invalid queue storage");
    check(pthread_mutex_lock(&registry));
    auto* q=queues;
    while(q && q->key!=queue) q=q->next;
    if(!q) {
        q=static_cast<Queue*>(std::calloc(1,sizeof(Queue)));
        if(!q) fail("queue allocation failed");
        q->key=queue; q->next=queues; queues=q;
        check(pthread_mutex_init(&q->mutex,nullptr));
        check(pthread_cond_init(&q->readable,nullptr)); check(pthread_cond_init(&q->writable,nullptr));
    }
    check(pthread_mutex_lock(&q->mutex));
    if(q->waiters) fail("reinitializing a queue with waiting threads");
    *queue=OSMessageQueue{};
    queue->msgArray=storage; queue->msgCount=capacity;
    check(pthread_mutex_unlock(&q->mutex)); check(pthread_mutex_unlock(&registry));
}
extern "C" BOOL OSSendMessage(OSMessageQueue* queue,OSMessage message,s32 flags) { return send(queue,message,flags,false); }
extern "C" BOOL OSJamMessage(OSMessageQueue* queue,OSMessage message,s32 flags) { return send(queue,message,flags,true); }
extern "C" BOOL OSReceiveMessage(OSMessageQueue* queue,OSMessage* output,s32 flags) {
    p2_thread_checkpoint();
    auto* q=find(queue); check(pthread_mutex_lock(&q->mutex));
    while(!queue->usedCount) {
        if(!(flags&OS_MESSAGE_BLOCK)) { check(pthread_mutex_unlock(&q->mutex)); return FALSE; }
        ++q->readers; wait(q,&q->readable); --q->readers;
    }
    if(output) *output=queue->msgArray[queue->firstIndex];
    queue->firstIndex=(queue->firstIndex+1)%queue->msgCount; --queue->usedCount;
    check(pthread_cond_signal(&q->writable)); check(pthread_mutex_unlock(&q->mutex));
    p2_thread_checkpoint();
    return TRUE;
}
extern "C" void p2_destroy_message_queue(void* key) {
    check(pthread_mutex_lock(&registry));
    auto** link=&queues;
    while(*link && (*link)->key!=key) link=&(*link)->next;
    if(*link) {
        auto* q=*link; check(pthread_mutex_lock(&q->mutex));
        if(q->waiters) fail("destroying a queue with waiting threads");
        check(pthread_mutex_unlock(&q->mutex));
        check(pthread_cond_destroy(&q->readable)); check(pthread_cond_destroy(&q->writable));
        check(pthread_mutex_destroy(&q->mutex)); *link=q->next; std::free(q);
    }
    check(pthread_mutex_unlock(&registry));
}
// A message sent to a queue a thread is waiting to read, not yet taken: the
// reader is about to run. Record/replay waits for none between ticks.
extern "C" int p2_messages_pending_for_waiters() {
    int pending=0;
    check(pthread_mutex_lock(&registry));
    for(auto* q=queues;q;q=q->next) {
        check(pthread_mutex_lock(&q->mutex));
        pending+=q->readers && q->key->usedCount;
        check(pthread_mutex_unlock(&q->mutex));
    }
    check(pthread_mutex_unlock(&registry));
    return pending;
}
