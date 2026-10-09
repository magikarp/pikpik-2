#include "p2_reset.h"
#include "System.h"
#include "Dolphin/os.h"
#include <mach-o/dyld.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {
RenderModeInfo video{};
static_assert(sizeof(video)==8,"Reset video state matches the game's saved region");
char* executable;
char** arguments;
void (*shutdownHandler)();
bool initialized,saveVideo;
constexpr const char* stateVariable="P2_NATIVE_RESET_VIDEO";
[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr,"Pikmin 2 reset: %s\n",message);std::abort();
}
int hex(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
void importVideo() {
    const char* encoded=std::getenv(stateVariable);
    if(!encoded) return;
    if(std::strlen(encoded)!=18||encoded[0]!='1'||encoded[1]!=':') fail("invalid video-state handoff");
    unsigned char bytes[8];
    for(unsigned i=0;i<8;++i) {
        const int hi=hex(encoded[2+2*i]),lo=hex(encoded[3+2*i]);
        if(hi<0||lo<0) fail("invalid video-state encoding");
        bytes[i]=static_cast<unsigned char>(hi*16+lo);
    }
    std::memcpy(&video,bytes,sizeof(video));
    if(video.mIdentifier!=0x76616c64u||video.mRenderMode>=4) fail("invalid saved video settings");
    if(unsetenv(stateVariable)) fail("cannot consume video-state handoff");
}
}
RenderModeInfo* p2_reset_video_state() { return &video; }
bool p2_reset_initialize(int argc,char** argv,void (*shutdown)()) {
    if(initialized||argc<1||!argv||!argv[0]) return false;
    uint32_t size=0;
    _NSGetExecutablePath(nullptr,&size);
    executable=static_cast<char*>(std::malloc(size));
    if(!executable||_NSGetExecutablePath(executable,&size)) fail("cannot resolve executable path");
    arguments=static_cast<char**>(std::calloc(static_cast<size_t>(argc)+1,sizeof(char*)));
    if(!arguments) fail("cannot preserve launch arguments");
    for(int i=0;i<argc;++i) {
        arguments[i]=strdup(argv[i]);
        if(!arguments[i]) fail("cannot preserve launch argument");
    }
    importVideo();shutdownHandler=shutdown;initialized=true;return true;
}
extern "C" void OSSetSaveRegion(void* start,void* end) {
    if(!start&&!end) { saveVideo=false;return; }
    // Pikmin 2 only saves this eight-byte video record. Reject other ranges
    // explicitly instead of retaining arbitrary host pointers across exec.
    if(start!=&video||end!=reinterpret_cast<unsigned char*>(&video)+sizeof(video))
        fail("unsupported save region");
    saveVideo=true;
}
extern "C" void OSGetSaveRegion(void** start,void** end) {
    if(start) *start=saveVideo ? &video : nullptr;
    if(end) *end=saveVideo ? reinterpret_cast<unsigned char*>(&video)+sizeof(video) : nullptr;
}
extern "C" void OSResetSystem(int reset,u32 code,BOOL forceMenu) {
    if(!initialized) fail("reset requested before process initialization");
    if(code) fail("nonzero console reset code is unsupported");
    if(reset<OS_RESET_RESTART||reset>OS_RESET_SHUTDOWN) fail("unknown reset type");
    if(unsetenv(stateVariable)) fail("cannot clear video-state handoff");
    if(!forceMenu && reset==OS_RESET_RESTART && saveVideo) {
        if(video.mIdentifier!=0x76616c64u||video.mRenderMode>=4) fail("invalid video settings at reset");
        constexpr char digits[]="0123456789abcdef";
        char encoded[19]={'1',':'};
        const auto* bytes=reinterpret_cast<const unsigned char*>(&video);
        for(unsigned i=0;i<8;++i) { encoded[2+i*2]=digits[bytes[i]>>4];encoded[3+i*2]=digits[bytes[i]&15]; }
        if(setenv(stateVariable,encoded,1)) fail("cannot preserve video settings");
    }
    if(shutdownHandler) shutdownHandler();
    std::fflush(nullptr);
    if(forceMenu||reset==OS_RESET_SHUTDOWN) std::exit(0);
    execv(executable,arguments);
    std::fprintf(stderr,"Pikmin 2 reset: relaunch failed: %s\n",std::strerror(errno));
    std::_Exit(127); // Threads and renderer were shut down; never resume the old game.
}
