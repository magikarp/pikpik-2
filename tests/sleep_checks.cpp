#include "Dolphin/os.h"
#include "p2_memory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
static std::atomic<bool> started{false},returned{false};
static void* sleeper(void*) {
    started=true;
    p2_thread_sleep_ticks(40500000*5LL);
    returned=true;return nullptr;
}
int main() {
    using Clock=std::chrono::steady_clock;
    p2_thread_sleep_ticks(0);p2_thread_sleep_ticks(-1);
    const auto before=Clock::now();p2_thread_sleep_ticks(405000);
    if(Clock::now()-before<std::chrono::milliseconds(10)) return 1;
    OSThread thread{};alignas(32) unsigned char stack[4096];
    if(!OSCreateThread(&thread,sleeper,nullptr,stack+sizeof(stack),sizeof(stack),16,0)) return 1;
    OSResumeThread(&thread);
    while(!started) std::this_thread::yield();
    const auto cancelled=Clock::now();OSCancelThread(&thread);
    const bool stopped=!returned&&Clock::now()-cancelled<std::chrono::seconds(2);
    p2_destroy_thread(&thread);
    std::printf("Native timed wait and cancellation: %s\n",stopped?"passed":"FAILED");return stopped?0:1;
}
