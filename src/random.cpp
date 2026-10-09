#include "p2_random.h"
#include <atomic>
namespace { std::atomic<uint32_t> state{1}; }
// Development trace (P2_TRACE_RNG, src/input_record.cpp): called with each caller.
extern "C" { void (*p2_game_rand_trace)(void* caller)=nullptr; }
extern "C" int p2_game_rand() {
    // Exact MSL generator and 15-bit result, with defined uint32 overflow.
    auto previous=state.load(std::memory_order_relaxed);
    uint32_t next;
    do { next=previous*1103515245u+12345u; }
    while(!state.compare_exchange_weak(previous,next,std::memory_order_relaxed));
    if(p2_game_rand_trace) p2_game_rand_trace(__builtin_return_address(0));
    return (next>>16)&0x7fff;
}
extern "C" void p2_game_srand(uint32_t seed) { state.store(seed,std::memory_order_relaxed); }
extern "C" uint32_t p2_game_rand_state() { return state.load(std::memory_order_relaxed); }
