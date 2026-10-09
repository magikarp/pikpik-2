// Input recording and replay (P2_INPUT_REC / P2_INPUT_PLAY), after Pikmin 1's
// pc_input_rec.cpp.
//
// The tick is the PADRead call: JUTGamePad::read is reached once per game frame
// from System::beginFrame, so its call count is the simulation's own clock.
// Input is indexed on it, not on wall time or presented frames.
//
// Initial conditions recorded with the input:
//   - the boot RNG seed (System::construct seeds from OSGetTick), and
//   - a fingerprint of the memory-card directory contents. Replaying against a
//     different save desyncs silently, so a mismatch is reported loudly.
// All host-side storage here uses P2HostScratchScope: global new allocates
// from the game's current JKR heap, which is nearly full in gameplay.
// Frame time is already fixed (System: mDeltaTime = mFrameRate / 60).
//
// P2_TRACE_TICKS=n prints the RNG state every n ticks: two runs that agree
// there consumed the same random sequence. P2_EXIT_TICK=n exits cleanly at a tick.
// P2_TRACE_RNG=a-b logs every RNG draw's caller in that tick range.
// P2_DUMP_TICKS=a,b,... writes P2_DUMP_DIR/tick-<n>.ppm for the frame built on each tick.
//
// File format v1, text, one line per CHANGE of one port's state:
//   tick port buttons stickX stickY substickX substickY trigL trigR
// A port that never appears is left as the host reports it (normally absent).
#include "p2_game_alloc.h"
#include "p2_random.h"
#include "p2_memory.h"
extern "C" void (*p2_game_rand_trace)(void* caller);
extern "C" int p2_dvd_pending(); // dvd_worker.cpp
extern "C" int p2_touch_pad_apply(void* pad_status); // touch_pad.cpp
extern "C" uint32_t p2_tick_now; // panic.cpp, read by diagnostics outside record/replay
#include <dolphin/pad.h>
#include <dlfcn.h>
#include <pthread.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

static_assert(sizeof(PADStatus)==16,"JUTGamePad's four-port array must share Aurora's stride");

namespace {
struct PortState {
    u16 buttons; s8 stickX, stickY, substickX, substickY; u8 triggerL, triggerR;
    bool operator==(const PortState&) const = default;
};
struct Change { uint32_t tick; int port; PortState state; };

bool initialized;
FILE* recording;
std::vector<Change> playback;
size_t playbackIndex;
bool playbackDone;
PortState lastState[PAD_CHANMAX];
bool haveLast[PAD_CHANMAX];
uint32_t tick;
uint32_t traceInterval;
uint32_t exitTick=UINT32_MAX;
uint32_t rngTraceFirst=1, rngTraceLast=0;
bool lockLoads; // record, replay or bench: loads complete on a fixed tick
std::vector<uint32_t> dumpTicks;
std::string dumpDirectory=".";

PortState capture(const PADStatus& pad) {
    return {pad.button, pad.stickX, pad.stickY, pad.substickX, pad.substickY, pad.triggerLeft, pad.triggerRight};
}
void apply(PADStatus& pad, const PortState& state) {
    pad.button=state.buttons; pad.stickX=state.stickX; pad.stickY=state.stickY;
    pad.substickX=state.substickX; pad.substickY=state.substickY;
    pad.triggerLeft=state.triggerL; pad.triggerRight=state.triggerR;
    pad.analogA=pad.analogB=0; pad.err=PAD_ERR_NONE;
}

std::filesystem::path cardDirectory() {
    const char* configured=std::getenv("P2_CARD_DIRECTORY");
    return configured&&*configured?configured:P2_CARD_ROOT;
}
// FNV-1a over sorted file names and contents of the card directory.
uint64_t saveFingerprint() {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::directory_iterator(cardDirectory(),error))
        if(entry.is_regular_file() && entry.path().filename().string()[0]!='.') files.push_back(entry.path());
    std::sort(files.begin(),files.end());
    uint64_t hash=1469598103934665603ull;
    auto mix=[&](unsigned char byte) { hash=(hash^byte)*1099511628211ull; };
    for(const auto& file:files) {
        for(char c:file.filename().string()) mix(static_cast<unsigned char>(c));
        std::ifstream in(file,std::ios::binary);
        for(char buffer[65536];in.read(buffer,sizeof(buffer))||in.gcount();)
            for(std::streamsize i=0;i<in.gcount();++i) mix(static_cast<unsigned char>(buffer[i]));
    }
    return hash;
}

uint32_t recordedSeed;
bool haveRecordedSeed;
void loadPlayback(const char* path) {
    FILE* file=std::fopen(path,"r");
    if(!file) { std::fprintf(stderr,"[INPUT] *** replay: cannot open '%s'\n",path); std::exit(1); }
    uint64_t recordedFingerprint=0;
    char line[256];
    while(std::fgets(line,sizeof(line),file)) {
        if(line[0]=='#') continue;
        unsigned long long value;
        if(std::sscanf(line,"save %llu",&value)==1) { recordedFingerprint=value; continue; }
        if(std::sscanf(line,"seed %llu",&value)==1) { recordedSeed=uint32_t(value); haveRecordedSeed=true; continue; }
        unsigned t,b,tl,tr; int port,sx,sy,cx,cy;
        if(std::sscanf(line,"%u %d %u %d %d %d %d %u %u",&t,&port,&b,&sx,&sy,&cx,&cy,&tl,&tr)!=9 || port<0 || port>=PAD_CHANMAX) continue;
        playback.push_back({t,port,{u16(b),s8(sx),s8(sy),s8(cx),s8(cy),u8(tl),u8(tr)}});
    }
    std::fclose(file);
    std::fprintf(stderr,"[INPUT] replay: %zu input changes from '%s', last tick %u\n",playback.size(),path,
        playback.empty()?0u:playback.back().tick);
    const uint64_t now=saveFingerprint();
    if(recordedFingerprint && now!=recordedFingerprint)
        std::fprintf(stderr,"[INPUT] *** SAVE STATE DIFFERS from the recording (%llu vs %llu); the replay WILL desync.\n",
            (unsigned long long)now,(unsigned long long)recordedFingerprint);
    else if(recordedFingerprint) std::fputs("[INPUT] replay: save state matches the recording\n",stderr);
    if(!haveRecordedSeed) std::fputs("[INPUT] *** replay: recording has no RNG seed; it WILL desync.\n",stderr);
}

void traceRandom(void* caller) {
    if(tick<rngTraceFirst || tick>rngTraceLast) return;
    Dl_info info{};
    const char* name=dladdr(caller,&info) && info.dli_sname ? info.dli_sname : "?";
    std::fprintf(stderr,"[RNG] tick %u %s %s+%td state %u\n",tick,pthread_main_np()?"main":"other",name,
        info.dli_saddr ? (char*)caller-(char*)info.dli_saddr : 0,p2_game_rand_state());
}
void initialize() {
    if(initialized) return;
    P2HostScratchScope hostStorage; // global new would otherwise allocate from the current game heap
    initialized=true;
    if(const char* interval=std::getenv("P2_TRACE_TICKS")) traceInterval=uint32_t(std::strtoul(interval,nullptr,10));
    if(const char* last=std::getenv("P2_EXIT_TICK")) exitTick=uint32_t(std::strtoul(last,nullptr,10));
    if(const char* range=std::getenv("P2_TRACE_RNG")) {
        // P2_TRACE_RNG=first-last: log the caller of every game RNG draw in that tick range.
        if(std::sscanf(range,"%u-%u",&rngTraceFirst,&rngTraceLast)==2) p2_game_rand_trace=traceRandom;
    }
    if(const char* list=std::getenv("P2_DUMP_TICKS"))
        for(char* end=nullptr;*list;list=*end?end+1:end) dumpTicks.push_back(uint32_t(std::strtoul(list,&end,10)));
    if(const char* directory=std::getenv("P2_DUMP_DIR")) dumpDirectory=directory;
    lockLoads=std::getenv("P2_INPUT_PLAY") || std::getenv("P2_INPUT_REC") || std::getenv("P2_BENCH");
    if(const char* path=std::getenv("P2_INPUT_PLAY")) {
        loadPlayback(path);
        // A bench replay ends at the last recorded input (what tools/bench.sh passes as P2_EXIT_TICK).
        if(std::getenv("P2_BENCH") && exitTick==UINT32_MAX && !playback.empty()) exitTick=playback.back().tick;
        return;
    }
    if(const char* path=std::getenv("P2_INPUT_REC")) {
        recording=std::fopen(path,"w");
        if(!recording) { std::fprintf(stderr,"[INPUT] *** record: cannot write '%s'\n",path); std::exit(1); }
        std::fprintf(recording,"# pikmin2-metal input recording v1\n"
            "# tick = PADRead call index (one per game frame)\n"
            "# fields: tick port buttons stickX stickY substickX substickY trigL trigR\n"
            "# only CHANGES are stored; a port's state holds until its next line\n"
            "save %llu\n",(unsigned long long)saveFingerprint());
        std::fflush(recording);
        std::fprintf(stderr,"[INPUT] recording to '%s'\n",path);
    }
}
}

// System::construct seeds the game RNG from OSGetTick. A replay must reuse the
// recording's seed; a recording stores the one it used.
extern "C" void p2_renderer_shutdown();
extern "C" void p2_aurora_request_frame_dump(const char* path);

extern "C" uint32_t p2_input_boot_seed(uint32_t natural) {
    initialize();
    if(!playback.empty() || haveRecordedSeed) {
        std::fprintf(stderr,"[INPUT] replay: RNG seed %u\n",recordedSeed);
        return recordedSeed;
    }
    if(const char* fixed=std::getenv("P2_RNG_SEED")) natural=uint32_t(std::strtoul(fixed,nullptr,0));
    if(recording) { std::fprintf(recording,"seed %u\n",natural); std::fflush(recording); }
    return natural;
}

// Called by JUTGamePad::read immediately after PADRead fills all four ports.
extern "C" void p2_input_tick(void* statuses) {
    initialize();
    auto* pads=static_cast<PADStatus*>(statuses);
    // Loads and other worker tasks draw from the game RNG (title Pikmin, the
    // card screen); finishing them before the tick makes their timing
    // independent of disk and frame speed. Work that needs the main thread to
    // progress is given up on after 5 s, loudly.
    if(lockLoads) {
        // Tick barrier: every game worker idle, no message waiting for a worker
        // that is about to run, no DVD command in flight.
        p2_main_block_begin(); // workers may start jobs queued this tick
        struct Unblock { ~Unblock() { p2_main_block_end(); } } unblock;
        for(int waited=0;p2_busy_workers()>0 || p2_messages_pending_for_waiters()>0 || p2_dvd_pending()>0;++waited) {
            if(waited==5000) {
                std::fprintf(stderr,"[INPUT] *** tick %u: workers still busy after 5 s (busy %d, messages %d, dvd %d); replay may desync\n",
                    tick,p2_busy_workers(),p2_messages_pending_for_waiters(),p2_dvd_pending());
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    const uint32_t now=tick++;
    p2_tick_now=tick;
    if(now>=exitTick) {
        std::fprintf(stderr,"[INPUT] P2_EXIT_TICK %u reached\n",now);
        p2_renderer_shutdown(); std::exit(0);
    }
    if(std::find(dumpTicks.begin(),dumpTicks.end(),now)!=dumpTicks.end()) {
        P2HostScratchScope hostStorage;
        const std::string path=dumpDirectory+"/tick-"+std::to_string(now)+".ppm";
        p2_aurora_request_frame_dump(path.c_str()); // written at the end of this frame
    }
    // Touch overlay (iOS): OR'd into port 1 before recording, so recordings
    // capture what the game read. With no controller the port reports
    // connected while the overlay is active.
    if(p2_touch_pad_apply(&pads[0])) pads[0].err=PAD_ERR_NONE;
    if(traceInterval && now%traceInterval==0)
        std::fprintf(stderr,"[INPUT] tick %u rng %u\n",now,p2_game_rand_state());
    if(!playback.empty()) {
        for(;playbackIndex<playback.size() && playback[playbackIndex].tick<=now;++playbackIndex) {
            const Change& change=playback[playbackIndex];
            lastState[change.port]=change.state; haveLast[change.port]=true;
        }
        if(playbackIndex==playback.size() && !playbackDone) {
            playbackDone=true;
            std::fprintf(stderr,"[INPUT] replay: recording exhausted at tick %u; input now neutral\n",now);
        }
        for(int port=0;port<PAD_CHANMAX;++port) {
            if(haveLast[port]) apply(pads[port],lastState[port]);
            else { const s8 err=pads[port].err; apply(pads[port],PortState{}); pads[port].err=err; } // live input ignored
        }
        return;
    }
    if(!recording) return;
    for(int port=0;port<PAD_CHANMAX;++port) {
        if(pads[port].err!=PAD_ERR_NONE) continue;
        const PortState state=capture(pads[port]);
        if(haveLast[port] && state==lastState[port]) continue;
        // Flushed per change: the session worth keeping usually ends in a crash.
        std::fprintf(recording,"%u %d %u %d %d %d %d %u %u\n",now,port,state.buttons,state.stickX,state.stickY,
            state.substickX,state.substickY,state.triggerL,state.triggerR);
        std::fflush(recording);
        lastState[port]=state; haveLast[port]=true;
    }
}

extern "C" uint32_t p2_input_current_tick() { return tick; }
