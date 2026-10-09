#include "Dolphin/pad.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void PADSetVirtualStatus(u32,const PADStatus*);
static void require(bool ok,const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main() {
    struct { unsigned char before[32]; PADStatus pads[4]; unsigned char after[32]; } output;
    std::memset(&output,0xa5,sizeof(output));
    require(PADInit(),"native PAD initialization");
    for(unsigned i=0;i<4;++i) {
        PADStatus input{}; input.button=PAD_BUTTON_A<<i; input.stickX=20+i; input.stickY=30+i;
        input.triggerLeft=40+i; input.triggerRight=50+i;
        PADSetVirtualStatus(i,&input);
    }
    PADRead(output.pads);
    for(unsigned i=0;i<4;++i) {
        const auto& pad=output.pads[i];
        require(pad.err==0 && pad.button==(PAD_BUTTON_A<<i),"four-channel button stride");
        require(pad.stickX==20+i && pad.stickY==30+i,"four-channel axis stride");
        require(pad.triggerLeft==40+i && pad.triggerRight==50+i,"four-channel trigger stride");
    }
    for(unsigned i=0;i<32;++i) require(output.before[i]==0xa5 && output.after[i]==0xa5,"PADRead preserves surrounding storage");
    std::puts("Original PADStatus storage matches native backend across all four channels; canaries intact.");
}
