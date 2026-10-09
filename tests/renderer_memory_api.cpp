#include <aurora/aurora.h>
#include <dolphin/os.h>
namespace aurora { extern AuroraConfig g_config; }
extern "C" {
void p2_test_renderer_os_init(unsigned bytes) { aurora::g_config.mem1Size=bytes; OSInit(); }
void* p2_test_renderer_physical(unsigned address) { return OSPhysicalToCached(address); }
unsigned p2_test_renderer_address(void* address) { return OSCachedToPhysical(address); }
unsigned p2_test_renderer_size() { return OSGetPhysicalMemSize(); }
unsigned p2_test_renderer_clock() { return __OSBusClock; }
void* p2_test_arena_low(unsigned size,unsigned align) { return OSAllocFromArenaLo(size,align); }
void* p2_test_arena_high(unsigned size,unsigned align) { return OSAllocFromArenaHi(size,align); }
}
