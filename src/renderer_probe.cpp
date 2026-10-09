#include "p2_renderer.h"
#include "p2_video.h"
#include <dolphin/vi.h>
#include <cstdio>

// Exercise the same lifecycle as the game, without claiming game/title output.
int main(int argc,char** argv) {
    if(!p2_renderer_initialize(argc,argv)) return 1;
    for(int frame=0;frame<3;++frame) {
        VISetBlack(frame<2);VIFlush();VIWaitForRetrace();
        p2_renderer_begin_frame();p2_renderer_end_frame();
    }
    std::fputs("Pikmin 2 renderer: Metal initialized; three probe frames submitted\n",stderr);
    p2_renderer_shutdown();return 0;
}
