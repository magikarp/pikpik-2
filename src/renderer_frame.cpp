#include "p2_renderer.h"
#include "p2_video.h"
#include "p2_game_alloc.h"
#include "p2_touch_pad.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h> // GXTexObj, used by aurora/texture.hpp
#include <aurora/texture.hpp>
#include <dolphin/vi.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_scancode.h>
#include <mach/mach.h>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
extern "C" void p2_aurora_set_present_black(bool);
extern std::atomic<long long> p2_retrace_sleep_ns;
namespace {
bool initialized,frameOpen;
std::thread::id renderThread;
void require(bool value,const char* message) {
    if(!value) { std::fprintf(stderr,"Pikmin 2 renderer: %s\n",message);std::abort(); }
}
void initializeKeyboard() {
    require(PADInit(),"controller initialization failed");
    u32 count=0;
    if(PADGetKeyButtonBindings(0,&count)) return; // Preserve an active saved layout.
    const PADKeyButtonBinding buttons[] = {
        {SDL_SCANCODE_SPACE,PAD_BUTTON_A}, {SDL_SCANCODE_ESCAPE,PAD_BUTTON_B},
        {SDL_SCANCODE_X,PAD_BUTTON_X}, {SDL_SCANCODE_Y,PAD_BUTTON_Y},
        {SDL_SCANCODE_RETURN,PAD_BUTTON_START}, {SDL_SCANCODE_E,PAD_TRIGGER_Z},
        {SDL_SCANCODE_Q,PAD_TRIGGER_L}, {SDL_SCANCODE_R,PAD_TRIGGER_R},
        {SDL_SCANCODE_UP,PAD_BUTTON_UP}, {SDL_SCANCODE_DOWN,PAD_BUTTON_DOWN},
        {SDL_SCANCODE_LEFT,PAD_BUTTON_LEFT}, {SDL_SCANCODE_RIGHT,PAD_BUTTON_RIGHT}
    };
    const PADKeyAxisBinding axes[] = {
        {SDL_SCANCODE_D,PAD_AXIS_LEFT_X_POS,0}, {SDL_SCANCODE_A,PAD_AXIS_LEFT_X_NEG,0},
        {SDL_SCANCODE_W,PAD_AXIS_LEFT_Y_POS,0}, {SDL_SCANCODE_S,PAD_AXIS_LEFT_Y_NEG,0},
        {SDL_SCANCODE_L,PAD_AXIS_RIGHT_X_POS,0}, {SDL_SCANCODE_J,PAD_AXIS_RIGHT_X_NEG,0},
        {SDL_SCANCODE_I,PAD_AXIS_RIGHT_Y_POS,0}, {SDL_SCANCODE_K,PAD_AXIS_RIGHT_Y_NEG,0},
        {SDL_SCANCODE_Q,PAD_AXIS_TRIGGER_L,0}, {SDL_SCANCODE_R,PAD_AXIS_TRIGGER_R,0}
    };
    for(const auto& binding:buttons) require(PADSetKeyButtonBinding(0,binding),"keyboard button binding failed");
    for(const auto& binding:axes) require(PADSetKeyAxisBinding(0,binding),"keyboard axis binding failed");
    PADSetKeyboardActive(0,TRUE);
}
// Development input script: P2_INPUT_SCRIPT="frame[-last]:INPUT,..." holds a pad
// input from frame through last on the render frame count (e.g. "200:A,
// 3000-3300:STICK_UP"). STICK_* push the main stick fully. P2_EXIT_FRAME quits
// cleanly after that many frames. Both are inert unless set.
u32 frameCount;
void scriptedInput(u32 frame,PADStatus& status) {
    static const char* script=std::getenv("P2_INPUT_SCRIPT");
    if(!script) return;
    static const struct { const char* name; u16 button; s8 x,y; } names[] = {
        {"A",PAD_BUTTON_A},{"B",PAD_BUTTON_B},{"X",PAD_BUTTON_X},{"Y",PAD_BUTTON_Y},
        {"START",PAD_BUTTON_START},{"Z",PAD_TRIGGER_Z},{"L",PAD_TRIGGER_L},{"R",PAD_TRIGGER_R},
        {"UP",PAD_BUTTON_UP},{"DOWN",PAD_BUTTON_DOWN},{"LEFT",PAD_BUTTON_LEFT},{"RIGHT",PAD_BUTTON_RIGHT},
        {"STICK_UP",0,0,127},{"STICK_DOWN",0,0,-127},{"STICK_LEFT",0,-127,0},{"STICK_RIGHT",0,127,0}};
    for(const char* p=script;*p;) {
        char* end=nullptr;
        const unsigned long first=std::strtoul(p,&end,10);
        require(end!=p,"P2_INPUT_SCRIPT expects frame[-last]:INPUT entries");
        unsigned long last=first;
        if(*end=='-') { const char* q=end+1; last=std::strtoul(q,&end,10); require(end!=q && last>=first,"bad P2_INPUT_SCRIPT range"); }
        require(*end==':',"P2_INPUT_SCRIPT expects frame[-last]:INPUT entries");
        const char* name=end+1;
        size_t length=std::strcspn(name,",");
        bool known=false;
        for(const auto& entry:names)
            if(std::strlen(entry.name)==length && !std::strncmp(entry.name,name,length)) {
                known=true;
                if(frame>=first && frame<=last) {
                    status.button|=entry.button;
                    if(entry.x) status.stickX=entry.x;
                    if(entry.y) status.stickY=entry.y;
                }
            }
        require(known,"P2_INPUT_SCRIPT names an unknown input");
        p=name+length; if(*p==',') ++p;
    }
}
using PerfClock=std::chrono::steady_clock;
double perfBeginSeconds,perfEndSeconds;
u32 perfWidth,perfHeight;
// Graphics and benchmark settings (all optional):
//   P2_WINDOW=WxH        window size in points (default 640x480)
//   P2_RENDER_SCALE=N    internal resolution = 640x480 x N, independent of the window
//   P2_MSAA=N            multisample count
//   P2_TEXTURE_PACK=dir  Dolphin-format replacement pack (tex1_WxH_hash_fmt.dds)
//   P2_TEXTURE_DUMP=1    let Aurora dump the game's textures (hash checks)
//   P2_BENCH=1           offscreen (frames render and submit, nothing is presented),
//                        no 60 Hz retrace pacing; prints a frame-time summary at exit. Replays index by tick, so a bench replay
//                        renders the same frames as a normal one, only faster.
//   P2_BENCH_PRESENT=1   with P2_BENCH: present every frame with vsync, so the display paces it
//   P2_BENCH_OUT=file    also append the summary line to this file
//   P2_BENCH_LABEL=text  prefix for that line
bool bench, benchPresent;
u32 renderWidth,renderHeight;
size_t texturePackEntries;
float renderScale;
u32 msaaSamples=1;
std::vector<float>* benchFrames; // milliseconds between frame starts, host memory
PerfClock::time_point benchLast, benchStart;
void benchFrame() {
    if(!bench) return;
    const auto now=PerfClock::now();
    if(!benchFrames) { benchFrames=new std::vector<float>(); benchFrames->reserve(1<<16); benchStart=now; }
    else benchFrames->push_back(std::chrono::duration<float,std::milli>(now-benchLast).count());
    benchLast=now;
}
void benchSummary() {
    if(!bench || !benchFrames || benchFrames->empty()) return;
    auto times=*benchFrames;
    const double seconds=std::chrono::duration<double>(benchLast-benchStart).count();
    std::sort(times.begin(),times.end());
    auto pct=[&](double p) { return times[std::min(times.size()-1,size_t(p*(times.size()-1)))]; };
    double sum=0; for(float t:times) sum+=t;
    // Peak physical footprint: what iOS counts against the app's memory limit.
    task_vm_info_data_t vm{};
    mach_msg_type_number_t vmCount=TASK_VM_INFO_COUNT;
    const bool haveVm=task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&vm),&vmCount)==KERN_SUCCESS;
    char line[512];
    std::snprintf(line,sizeof(line),"frames %zu, %.2f s, %.1f fps | frame ms avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f"
        " | render %ux%u (scale %.2g, msaa %u), %s %ux%u, texture pack %zu entries | peak footprint %.0f MB",
        times.size(),seconds,times.size()/seconds,sum/times.size(),pct(0.5),pct(0.95),pct(0.99),times.back(),
        renderWidth,renderHeight,renderScale,msaaSamples,benchPresent?"presented":"offscreen, window",perfWidth,perfHeight,
        texturePackEntries,haveVm?vm.ledger_phys_footprint_peak/1048576.0:0.0);
    // iOS: what the app could use at launch (its memory limit minus what it already held).
    if(const char* available=std::getenv("P2_LAUNCH_AVAILABLE_MB"))
        std::snprintf(line+std::strlen(line),sizeof(line)-std::strlen(line),", %s MB available at launch",available);
    std::fprintf(stderr,"[BENCH] %s\n",line);
    if(const char* out=std::getenv("P2_BENCH_OUT"))
        if(FILE* f=std::fopen(out,"a")) {
            const char* label=std::getenv("P2_BENCH_LABEL");
            std::fprintf(f,"%s%s%s\n",label?label:"",label?" | ":"",line); std::fclose(f);
        }
}
// P2_PERF=1 reports frames per second and where each frame's time goes:
// Aurora frame acquire/present, the 60 Hz retrace sleep, and everything else
// (game update/draw submission), plus the presented framebuffer size.
void reportPerf() {
    static const bool enabled=std::getenv("P2_PERF")!=nullptr;
    if(!enabled) return;
    static auto windowStart=PerfClock::now();
    static u32 windowFrames=0;
    static long long sleepStart=p2_retrace_sleep_ns.load();
    ++windowFrames;
    const double seconds=std::chrono::duration<double>(PerfClock::now()-windowStart).count();
    if(seconds<2.0) return;
    const double sleep=(p2_retrace_sleep_ns.load()-sleepStart)/1e9;
    const double ms=1000.0/windowFrames;
    std::fprintf(stderr,"P2_PERF presented %.1f/s, game %.1f frames/s | per frame %.1f ms: acquire %.1f, present %.1f, retrace sleep %.1f, game %.1f | framebuffer %ux%u\n",
        aurora_get_fps(),windowFrames/seconds,seconds*ms,perfBeginSeconds*ms,perfEndSeconds*ms,sleep*ms,
        (seconds-perfBeginSeconds-perfEndSeconds-sleep)*ms,perfWidth,perfHeight);
    windowStart=PerfClock::now(); windowFrames=0; sleepStart=p2_retrace_sleep_ns.load();
    perfBeginSeconds=perfEndSeconds=0;
}
void touchFrame() {
    // Layout and finger coordinates are in the surface's unit space; a
    // connected controller hides the overlay. Wall clock, so the idle dim keeps
    // running while the game is paused.
    if(perfHeight) p2_touch_pad_set_aspect(float(perfWidth)/float(perfHeight));
    p2_touch_pad_set_controller(PADCount()>0);
    static PerfClock::time_point last;
    const auto now=PerfClock::now();
    if(last.time_since_epoch().count()) p2_touch_pad_advance(std::chrono::duration<float>(now-last).count());
    last=now;
}
void events() {
    PADStatus keyPress{};
    scriptedInput(++frameCount,keyPress);
    reportPerf();
    touchFrame();
    static const char* exitFrame=std::getenv("P2_EXIT_FRAME");
    if(exitFrame && frameCount>=std::strtoul(exitFrame,nullptr,10)) { p2_renderer_shutdown();std::exit(0); }
    for(const AuroraEvent* event=aurora_update();event&&event->type!=AURORA_NONE;++event) {
        if(event->type==AURORA_EXIT) { p2_renderer_shutdown();std::exit(0); }
        if(event->type==AURORA_WINDOW_RESIZED) {
            perfWidth=event->windowSize.native_fb_width;perfHeight=event->windowSize.native_fb_height;
            renderWidth=event->windowSize.fb_width;renderHeight=event->windowSize.fb_height;
        }
        if(event->type==AURORA_SDL_EVENT && event->sdl.type==SDL_EVENT_KEY_DOWN && !event->sdl.key.repeat &&
           !(event->sdl.key.mod & (SDL_KMOD_GUI|SDL_KMOD_CTRL|SDL_KMOD_ALT))) {
            u32 count=0;
            const auto* bindings=PADGetKeyButtonBindings(0,&count);
            for(u32 i=0;bindings && i<count;++i)
                if(bindings[i].scancode==event->sdl.key.scancode) keyPress.button|=bindings[i].padButton;
        }
        if(event->type==AURORA_SDL_EVENT && event->sdl.type>=SDL_EVENT_FINGER_DOWN && event->sdl.type<=SDL_EVENT_FINGER_CANCELED)
            p2_touch_pad_handle_event(&event->sdl);
        if(event->type==AURORA_PAUSED ||
           (event->type==AURORA_SDL_EVENT && event->sdl.type==SDL_EVENT_WINDOW_FOCUS_LOST)) keyPress={};
    }
    // Key-down and key-up can both be drained before SDL's state is polled.
    // Retain a real press until the next original System::beginFrame/PADRead.
    // Held keys still use the backend's keyboard state; this is only the edge.
    if(keyPress.button || keyPress.stickX || keyPress.stickY) PADSetVirtualStatus(0,&keyPress);
    else PADClearVirtualStatus(0);
}
}
extern "C" void p2_aurora_set_offscreen(bool offscreen);
extern "C" void p2_touch_overlay_install(); // src/touch_draw.cpp
extern "C" int p2_renderer_initialize(int argc,char** argv) {
    P2HostScratchScope hostStorage;
    require(!initialized,"initialized twice");
    AuroraConfig config{};
    config.appName="Pikmin 2 Metal";config.desiredBackend=BACKEND_METAL;
    config.logLevel=LOG_INFO;
    config.logCallback=[](AuroraLogLevel level,const char* module,const char* message,unsigned len) {
        static std::atomic<unsigned> count{0};
        std::fprintf(stderr,"[%d] [%s] %.*s\n",int(level),module,int(len),message);
        if(count.fetch_add(1)<500) if(FILE* f=std::fopen(P2_RENDERER_LOG_PATH,"a")) {
            std::fprintf(f,"[%d] [%s] %.*s\n",int(level),module,int(len),message); std::fclose(f);
        }
    };
    config.windowWidth=640;config.windowHeight=480;config.vsync=true;
    if(const char* window=std::getenv("P2_WINDOW")) {
        unsigned w=0,h=0;
        require(std::sscanf(window,"%ux%u",&w,&h)==2 && w>=160 && h>=120,"P2_WINDOW expects WxH");
        config.windowWidth=w;config.windowHeight=h;
    }
    if(const char* msaa=std::getenv("P2_MSAA")) msaaSamples=std::max(1ul,std::strtoul(msaa,nullptr,10));
    config.msaa=msaaSamples;
    config.allowTextureDumps=std::getenv("P2_TEXTURE_DUMP")!=nullptr;
    bench=std::getenv("P2_BENCH")!=nullptr;
    benchPresent=bench && std::getenv("P2_BENCH_PRESENT")!=nullptr;
    // The window still shows; nothing is presented to it while benchmarking offscreen.
    if(bench && !benchPresent) config.vsync=false;
    config.mem1Size=128*1024*1024;config.mem2Size=16*1024*1024;
    const auto info=aurora_initialize(argc,argv,&config);
    initialized=true;renderThread=std::this_thread::get_id();
    perfWidth=info.windowSize.native_fb_width;perfHeight=info.windowSize.native_fb_height;
    renderWidth=info.windowSize.fb_width;renderHeight=info.windowSize.fb_height;
    if(info.backend!=BACKEND_METAL||!info.window) { p2_renderer_shutdown();return 0; }
    if(bench && !benchPresent) p2_aurora_set_offscreen(true);
    if(const char* scale=std::getenv("P2_RENDER_SCALE"); scale && *scale) {
        renderScale=std::strtof(scale,nullptr);
        require(renderScale>0 && renderScale<=8,"P2_RENDER_SCALE expects 0 < N <= 8");
        VISetFrameBufferScale(renderScale);
    }
    if(const char* pack=std::getenv("P2_TEXTURE_PACK"); pack && *pack) { // empty = off
        const auto loadStart=PerfClock::now();
        texturePackEntries=aurora::texture::load_replacement_directory(pack).registrations.size();
        std::fprintf(stderr,"[GFX] texture pack %s: %zu replacements registered in %.2f s\n",pack,texturePackEntries,
            std::chrono::duration<double>(PerfClock::now()-loadStart).count());
    }
    std::fprintf(stderr,"[GFX] window %ux%u, render scale %s, msaa %u%s (render size is reported in the bench summary)\n",
        config.windowWidth,config.windowHeight,renderScale>0?std::to_string(renderScale).c_str():"window",msaaSamples,
        bench?", bench (uncapped)":"");
    p2_touch_overlay_install();
    OSInit(); // Establish the shared arena before System reads low memory.
    initializeKeyboard();
    return 1;
}
extern "C" void p2_renderer_begin_frame() {
    P2HostScratchScope hostStorage;
    require(initialized,"begin-frame before renderer initialization");
    require(renderThread==std::this_thread::get_id(),"frame called outside the render thread");
    // Some loaders call beginRender/endRender without endFrame (e.g.
    // BaseGameSection::setupFixMemory). On console the next beginRender copies
    // the pending frame to the display, so present it here instead of failing.
    if(frameOpen) p2_renderer_end_frame();
    benchFrame();
    do {
        events();
        const auto acquireStart=PerfClock::now();
        const bool acquired=aurora_begin_frame();
        perfBeginSeconds+=std::chrono::duration<double>(PerfClock::now()-acquireStart).count();
        if(acquired) { frameOpen=true;return; }
        // A minimized/paused window cannot acquire a surface. Keep pumping the
        // window until it can render; do not issue game draws without a frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    } while(true);
}
extern "C" void p2_renderer_end_frame() {
    P2HostScratchScope hostStorage;
    require(initialized&&frameOpen,"end-frame without an open frame");
    require(renderThread==std::this_thread::get_id(),"frame called outside the render thread");
    p2_aurora_set_present_black(p2_video_is_black()!=0);
    const auto presentStart=PerfClock::now();
    aurora_end_frame();frameOpen=false;
    perfEndSeconds+=std::chrono::duration<double>(PerfClock::now()-presentStart).count();
}
// Services that use SDL (real audio output) stop before shutdown tears it down.
namespace { void (*shutdownHook)(); }
extern "C" void p2_renderer_set_shutdown_hook(void (*hook)()) { shutdownHook=hook; }
extern "C" void p2_renderer_shutdown() {
    P2HostScratchScope hostStorage;
    if(!initialized) return;
    if(shutdownHook) { auto hook=shutdownHook; shutdownHook=nullptr; hook(); }
    benchSummary();
    if(frameOpen) p2_renderer_end_frame();
    aurora_shutdown();initialized=false;
}
