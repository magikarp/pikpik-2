#include "p2_panic.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <unistd.h>

// Called by game assertions, not installed as a signal handler. Native faults
// retain the operating system's crash handling and register reporting.
extern "C" void p2_panic_v(const char* file,int line,const char* format,va_list* args) {
    // GUI launches may send stderr to /dev/null. Preserve the actual assertion
    // and allocation details in the build's diagnostic log without game new.
    FILE* saved=std::fopen(P2_PANIC_LOG_PATH,"a");
    if(saved) {
        va_list copy; va_copy(copy,*args);
        std::fprintf(saved,"PID %d: Pikmin 2 panic at %s:%d: ",getpid(),file?file:"<unknown>",line);
        std::vfprintf(saved,format?format:"<no message>",copy);
        va_end(copy); std::fputc('\n',saved); std::fflush(saved);
    }
    std::fprintf(stderr,"Pikmin 2 panic at %s:%d: ",file?file:"<unknown>",line);
    std::vfprintf(stderr,format?format:"<no message>",*args);
    std::fputs("\nNative stack trace:\n",stderr);
    std::fflush(stderr);
    void* frames[64];
    const int count=backtrace(frames,64);
    backtrace_symbols_fd(frames,count,STDERR_FILENO);
    if(saved) { backtrace_symbols_fd(frames,count,fileno(saved)); std::fclose(saved); }
    std::abort();
}

__attribute__((noreturn)) static void panicf(const char* file,int line,const char* format,...) {
    va_list args; va_start(args,format); p2_panic_v(file,line,format,&args);
}
extern "C" void p2_unused_function(const char* file,int line,const char* name) {
    panicf(file,line,"%s has no code in the original game but was called",name);
}

// Current input tick, published by record/replay; 0 without it.
extern "C" uint32_t p2_tick_now = 0;
