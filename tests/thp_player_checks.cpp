#include "THP/THPPlayer.h"
#include "THP/THPDraw.h"
#include "p2_dvd.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" void p2_thp_audio_mix(float* stereo, unsigned frames, unsigned rate); // thp_player.cpp
static void require(bool ok,const char* message) { if(!ok) { std::fprintf(stderr,"THP player: %s\n",message); std::exit(1); } }
static unsigned draws=0,fences=0;
// Playback/ownership checks are CPU-only. Production links the game's original
// THPDraw.c; these record its boundary without claiming GPU rendering.
extern "C" void THPGXYuv2RgbSetup(GXRenderModeObj* mode) { require(mode,"render mode"); }
extern "C" void THPGXYuv2RgbDraw(u32* y,u32* u,u32* v,s16,s16,s16 w,s16 h,s16 pw,s16 ph) {
    require(y && u && v && w==608 && h==448 && pw==608 && ph==448,"real movie draw arguments"); ++draws;
}
extern "C" void THPGXRestore() {}
extern "C" void GXDrawDone() { ++fences; }
int main(int argc,char** argv) {
    require(argc==2 && p2_dvd_mount(argv[1]),"mount disc");
    require(!THPPlayerOpen("/thp/play1.thp",FALSE),"init required");
    require(THPPlayerInit(0),"initialize");
    require(!THPPlayerOpen("/thp/missing.thp",FALSE),"missing file rejected");
    require(!THPPlayerOpen("/thp/play1.thp",TRUE),"unsupported preload rejected");
    require(THPPlayerOpen("/thp/play1.thp",FALSE),"open movie");
    THPVideoInfo vi{}; THPAudioInfo ai{};
    require(THPPlayerGetVideoInfo(&vi) && vi.mXSize==608 && vi.mYSize==448,"video metadata");
    require(THPPlayerGetAudioInfo(&ai) && ai.mSndNumTracks>0,"audio metadata retained");
    require(!THPPlayerPrepare(0,0,0),"buffer required");
    const auto size=THPPlayerCalcNeedMemory();
    std::vector<u8> storage(size+65,0x5a);
    require(THPPlayerSetBuffer(storage.data()+1),"unaligned caller buffer supported");
    require(!THPPlayerPrepare(-1,0,0) && !THPPlayerPrepare(1200,0,0),"invalid frame rejected");
    require(THPPlayerPrepare(0,0,0) && ActivePlayer.mState==1,"prepare first frame");
    require(THPPlayerSetVolume(64,0) && ActivePlayer.mCurVolume==64 && !THPPlayerSetVolume(128,0),"movie volume stored for the mixer");
    GXRenderModeObj mode{};
    require(THPPlayerDrawCurrentFrame(&mode,0,0,608,448)==0,"prepared frame available");
    require(!THPPlayerClose(),"active close rejected");
    require(THPPlayerPlay(),"play");
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    {   // Movie audio decodes for the real-audio mixer: half a second at 32 kHz.
        std::vector<float> stereo(32000,0.0f);
        p2_thp_audio_mix(stereo.data(),16000,32000);
        float peak=0; for(float s:stereo) peak=std::max(peak,std::fabs(s));
        require(peak>0.001f && peak<=1.0f,"movie audio decodes");
    }
    require(THPPlayerPause(),"pause");
    // Paused drawing holds the last decoded image; playback resumes at its
    // retained elapsed time rather than restarting the movie.
    require(THPPlayerDrawCurrentFrame(&mode,0,0,608,448)==0,"pause holds image");
    require(THPPlayerPlay(),"resume");
    const int advanced=THPPlayerDrawCurrentFrame(&mode,0,0,608,448);
    require(advanced>0 && advanced<100 && fences>0,"clock advance and prior image consumption");
    THPPlayerDrawDone();
    THPPlayerStop();
    require(THPPlayerPrepare(1199,0,0),"seek final frame");
    require(THPPlayerPlay(),"play final frame");
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    require(THPPlayerDrawCurrentFrame(&mode,0,0,608,448)==1199 && ActivePlayer.mState==3,"natural completion");
    THPPlayerStop();
    require(THPPlayerPrepare(1199,1,0) && THPPlayerPlay(),"loop setup");
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    const auto looped=THPPlayerDrawCurrentFrame(&mode,0,0,608,448);
    require(looped>=0 && looped<100 && ActivePlayer.mState==2,"loop wraps to start");
    THPPlayerStop(); require(THPPlayerClose(),"close after stop");
    require(storage[0]==0x5a,"leading buffer guard");
    for(size_t i=size+1;i<storage.size();++i) require(storage[i]==0x5a,"trailing buffer guard");
    require(THPPlayerOpen("/thp/play1.thp",FALSE),"reopen");
    THPPlayerQuit();
    require(!ActivePlayer.mIsOpen && !THPPlayerPlay(),"quit closes and clears state");
    require(draws>=5 && fences>=3,"draw/consumption lifecycle exercised");
    std::printf("Real streamed THP playback, pause/resume, end, loop and cleanup passed.\n");
}
