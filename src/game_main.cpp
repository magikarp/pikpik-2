#include "p2_renderer.h"
#include "p2_reset.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int p2_game_main();
extern "C" void p2_crash_handler_install(void);
extern "C" void p2_thp_use_retrace_clock(unsigned (*retraces)());
extern "C" unsigned VIGetRetraceCount();
int main(int argc,char** argv) {
    p2_crash_handler_install();
    std::fprintf(stderr,"Pikmin 2 native process: %ld\n",static_cast<long>(getpid()));
    if(!p2_reset_initialize(argc,argv,p2_renderer_shutdown)) return 1;
    const char* root=std::getenv("P2_DISC_DIRECTORY");
    if(!p2_dvd_mount(root&&*root?root:P2_DISC_ROOT)) {
        std::fputs("Pikmin 2: unable to mount extracted disc\n",stderr);return 1;
    }
    if(!p2_renderer_initialize(argc,argv)) return 1;
    p2_thp_use_retrace_clock(VIGetRetraceCount); // movies advance with game retraces
    const int result=p2_game_main();
    p2_renderer_shutdown();return result;
}
