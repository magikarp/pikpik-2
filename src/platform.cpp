#include "Dolphin/os.h"
#include "Dolphin/PPCArch.h"
#include "Dolphin/gba.h"
#include <atomic>

namespace {
// Native bring-up settings are process-local. The game still owns its saved
// options; emulated console SRAM persistence has not been introduced.
std::atomic<u32> soundMode{OS_SOUND_MODE_STEREO};
std::atomic<u32> progressiveMode{OS_PROGRESSIVE_MODE_OFF};
std::atomic<u32> euRgb60Mode{OS_EURGB60_OFF};
}
extern "C" u32 OSGetSoundMode() { return soundMode.load(); }
extern "C" void OSSetSoundMode(u32 mode) { soundMode.store(mode&1); }
extern "C" u32 OSGetProgressiveMode() { return progressiveMode.load(); }
extern "C" void OSSetProgressiveMode(u32 mode) { progressiveMode.store(mode&1); }
extern "C" u32 OSGetEuRgb60Mode() { return euRgb60Mode.load(); }
extern "C" void OSSetEuRgb60Mode(u32 mode) { euRgb60Mode.store(mode&1); }
// A Mac has no console reset switch. Controller reset detection remains in the
// original JUTGamePad/ResetManager path and does not depend on this switch.
extern "C" BOOL OSGetResetSwitchState() { return FALSE; }

// CPU storage in the native port is coherent, including MEM1 and staged GX
// command/vertex data. There are no emulated dirty console cache lines to flush
// or invalidate. Retain ordering at the SDK boundary; submission/queue locks
// establish cross-thread ownership, and Aurora owns GPU uploads/completion.
extern "C" void DCFlushRange(void*,u32) { std::atomic_thread_fence(std::memory_order_seq_cst); }
extern "C" void DCStoreRange(void*,u32) { std::atomic_thread_fence(std::memory_order_seq_cst); }
extern "C" void DCInvalidateRange(void*,u32) { std::atomic_thread_fence(std::memory_order_seq_cst); }
extern "C" void DCFlushRangeNoSync(void*,u32) { std::atomic_signal_fence(std::memory_order_seq_cst); }
extern "C" void DCStoreRangeNoSync(void*,u32) { std::atomic_signal_fence(std::memory_order_seq_cst); }
extern "C" void PPCSync() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// No GBA link transport is attached to the native port. The pinned SDK's
// TypeAndStatusCallback returns 1 for an absent/wrong SI device, and its
// command handlers leave caller buffers unchanged on failure. Report that
// state immediately so the original e-Reader UI can take its failure path.
extern "C" void GBAInit() {}
extern "C" int GBAReset(s32, u8*) { return 1; }
extern "C" int GBAGetStatus(s32, u8*) { return 1; }
extern "C" int GBARead(s32, u8*, u8*) { return 1; }
extern "C" int GBAWrite(s32, u8*, u8*) { return 1; }
