#include "p2_game_alloc.h"

// Depth of P2HostScratchScope on this thread. While nonzero, global new
// without an explicit heap uses host memory instead of the current JKR heap.
namespace { thread_local unsigned hostScratchDepth=0; }
P2HostScratchScope::P2HostScratchScope() { ++hostScratchDepth; }
P2HostScratchScope::~P2HostScratchScope() { --hostScratchDepth; }
bool p2_host_scratch_active() { return hostScratchDepth!=0; }
// C hooks for code that cannot see the class (Aurora's input library).
extern "C" void p2_host_scratch_push() { ++hostScratchDepth; }
extern "C" void p2_host_scratch_pop() { --hostScratchDepth; }
// Called from stb::TObject's destructor (tools/prepare_source.py): being opaque
// to the compiler, it keeps that destructor's vtable store, as MWCC does.
extern "C" void p2_keep_destroyed_vtable(void*) {}
