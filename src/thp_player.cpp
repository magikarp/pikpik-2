#include "THP/THPPlayer.h"
#include "THP/THPDraw.h"
#include "p2_thp_decode.h"
#include "p2_game_alloc.h"
#include <dolphin/thp.h>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

extern "C" { THPPlayer ActivePlayer{}; u8 gTHPReaderDvdAccess=0; }
namespace {
std::mutex mutex;
bool initialized=false,submitted=false;
u8* compressed=nullptr;
u32 cursor=0,cursorSize=0,cursorFrame=0;
size_t yBytes=0,cBytes=0;
using Clock=std::chrono::steady_clock;
Clock::time_point started;
double elapsed=0;
// Playback clock. The console player advanced on VI retraces; the game app
// installs the retrace counter (p2_thp_use_retrace_clock) so movies stay locked
// to game ticks (record/replay, uncapped benchmarks). Wall time otherwise.
u32 (*retraceClock)()=nullptr;
u32 startedRetrace=0;
u32 be(const u8* p) { return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3]; }
size_t aligned(size_t n) { return (n+31)&~size_t(31); }
bool read(void* dst,u32 size,u32 offset) {
    auto& file=ActivePlayer.mFileInfo;
    if(offset>file.length || size>file.length-offset || size>0x7fffffff || offset>0x7fffffff) return false;
    gTHPReaderDvdAccess=1;
    const auto result=DVDReadPrio(&file,dst,size,offset,2);
    gTHPReaderDvdAccess=0;
    if(result!=s32(size)) { ActivePlayer.mDvdError=-1; return false; }
    return true;
}
void finishDraw() {
    // Aurora consumes the texture bytes before this command-processing fence.
    // This is not a claim of GPU completion; subsequent uploads own their data.
    if(submitted) { GXDrawDone(); submitted=false; }
}
void stop() {
    finishDraw();
    ActivePlayer.mState=0;
    ActivePlayer.mDispTextureSet=nullptr;
    ActivePlayer.mDvdError=ActivePlayer.mVideoError=0;
    elapsed=0;
}
void close() {
    stop();
    if(ActivePlayer.mIsOpen) DVDClose(&ActivePlayer.mFileInfo);
    ActivePlayer={}; compressed=nullptr;
}
bool decode(u32 frame) {
    const auto& h=ActivePlayer.mHeader;
    if(frame>=h.mNumFrames || !compressed) return false;
    if(frame<cursorFrame) { cursor=h.mMovieDataOffsets; cursorSize=h.mFirstFrameSize; cursorFrame=0; }
    const u32 headerSize=8+4*ActivePlayer.mCompInfo.mNumComponents;
    while(true) {
        const auto end=std::uint64_t(h.mMovieDataOffsets)+h.mMovieDataSize;
        if(cursorSize<headerSize || cursorSize>h.mBufferSize || std::uint64_t(cursor)+cursorSize>end) return false;
        if(cursorFrame==frame) break;
        u8 next[4];
        if(!read(next,4,cursor)) return false;
        cursor+=cursorSize; cursorSize=be(next); ++cursorFrame;
    }
    if(!read(compressed,cursorSize,cursor)) return false;
    size_t componentOffset=headerSize;
    const u8* video=nullptr; size_t videoSize=0;
    for(u32 i=0;i<ActivePlayer.mCompInfo.mNumComponents;++i) {
        const auto bytes=be(compressed+8+4*i);
        if(bytes>cursorSize-componentOffset) return false;
        if(ActivePlayer.mCompInfo.mFrameComp[i]==0) { video=compressed+componentOffset; videoSize=bytes; }
        componentOffset+=bytes;
    }
    auto& texture=ActivePlayer.mTextureSet[0];
    finishDraw();
    const auto result=p2_thp_decode(video,videoSize,texture.mYTexture,yBytes,
        texture.mUTexture,cBytes,texture.mVTexture,cBytes,
        ActivePlayer.mVideoInfo.mXSize,ActivePlayer.mVideoInfo.mYSize);
    if(result) { ActivePlayer.mVideoError=result; return false; }
    texture.mFrameNumber=frame;
    ActivePlayer.mDispTextureSet=&texture;
    ActivePlayer.mCurVideoNumber=frame;
    ++ActivePlayer.mVideoDecodeCount;
    return true;
}
double position() {
    if(ActivePlayer.mState!=2) return elapsed;
    if(retraceClock) return elapsed+double(retraceClock()-startedRetrace)/59.94;
    return elapsed+std::chrono::duration<double>(Clock::now()-started).count();
}

// Movie audio. The video clock picks frames from wall time; audio walks the
// frames in order from the prepared frame, decoding each frame's audio
// component into a stereo queue that the real-audio mixer pulls.
u32 audioCursor=0,audioCursorSize=0,audioFrame=0;
std::vector<s16> audioQueue; // interleaved stereo at mSndFrequency
size_t audioRead=0;
double audioPhase=0;
std::vector<u8> audioFrameBytes;
void resetAudio(u32 frame) {
    P2HostScratchScope hostStorage;
    audioCursor=ActivePlayer.mHeader.mMovieDataOffsets; audioCursorSize=ActivePlayer.mHeader.mFirstFrameSize; audioFrame=0;
    audioQueue.clear(); audioRead=0; audioPhase=0;
    // Walk to the start frame by frame sizes.
    while(audioFrame<frame) {
        u8 next[4];
        if(!read(next,4,audioCursor)) return;
        audioCursor+=audioCursorSize; audioCursorSize=be(next); ++audioFrame;
    }
}
bool decodeAudioFrame() {
    P2HostScratchScope hostStorage;
    const auto& h=ActivePlayer.mHeader;
    if(audioFrame>=h.mNumFrames) {
        if(!ActivePlayer.mPlayFlag) return false;
        resetAudio(0);
    }
    const u32 headerSize=8+4*ActivePlayer.mCompInfo.mNumComponents;
    if(audioCursorSize<headerSize || audioCursorSize>h.mBufferSize) return false;
    audioFrameBytes.resize(audioCursorSize);
    if(!read(audioFrameBytes.data(),audioCursorSize,audioCursor)) return false;
    const u8* frame=audioFrameBytes.data();
    size_t offset=headerSize;
    for(u32 i=0;i<ActivePlayer.mCompInfo.mNumComponents;++i) {
        const u32 bytes=be(frame+8+4*i);
        if(bytes>audioCursorSize-offset) return false;
        if(ActivePlayer.mCompInfo.mFrameComp[i]==1 && bytes) {
            const size_t start=audioQueue.size();
            audioQueue.resize(start+2*size_t(ActivePlayer.mHeader.mAudioMaxSamples)+32);
            const u32 samples=THPAudioDecode(audioQueue.data()+start,frame+offset,0);
            audioQueue.resize(start+2*size_t(samples));
        }
        offset+=bytes;
    }
    audioCursor+=audioCursorSize; audioCursorSize=be(frame); ++audioFrame;
    return true;
}
}
extern "C" {
BOOL THPPlayerInit(int) { std::lock_guard<std::mutex> lock(mutex); initialized=true; return TRUE; }
void THPPlayerQuit() { std::lock_guard<std::mutex> lock(mutex); close(); initialized=false; }
BOOL THPPlayerOpen(const char* name,BOOL onMemory) {
    std::lock_guard<std::mutex> lock(mutex);
    // The game's caller streams from disc. A whole-movie preload has no native
    // implementation yet, so reject it explicitly rather than misreport it.
    if(!initialized || ActivePlayer.mIsOpen || onMemory || !name) return FALSE;
    if(!DVDOpen(const_cast<char*>(name),&ActivePlayer.mFileInfo)) return FALSE;
    ActivePlayer.mIsOpen=TRUE;
    auto fail=[]() { close(); return FALSE; };
    u8 raw[48]; if(!read(raw,48,0) || std::memcmp(raw,"THP\0",4) || be(raw+4)!=0x11000) return fail();
    auto& h=ActivePlayer.mHeader;
    std::memcpy(h.mMagic,raw,4);
    h.mVersion=be(raw+4); h.mBufferSize=be(raw+8); h.mAudioMaxSamples=be(raw+12);
    const u32 fps=be(raw+16); std::memcpy(&h.mFrameRate,&fps,4);
    h.mNumFrames=be(raw+20); h.mFirstFrameSize=be(raw+24); h.mMovieDataSize=be(raw+28);
    h.mCompInfoDataOffsets=be(raw+32); h.mOffsetDataOffsets=be(raw+36);
    h.mMovieDataOffsets=be(raw+40); h.mFinalFrameDataOffsets=be(raw+44);
    if(!std::isfinite(h.mFrameRate) || h.mFrameRate<=0 || h.mFrameRate>120 ||
       !h.mNumFrames || h.mNumFrames>1000000 || !h.mBufferSize || h.mBufferSize>16*1024*1024 ||
       std::uint64_t(h.mMovieDataOffsets)+h.mMovieDataSize>ActivePlayer.mFileInfo.length ||
       h.mFirstFrameSize>h.mBufferSize || h.mMovieDataOffsets<48) return fail();
    if(!read(raw,20,h.mCompInfoDataOffsets)) return fail();
    auto& comp=ActivePlayer.mCompInfo; comp.mNumComponents=be(raw);
    if(comp.mNumComponents<1 || comp.mNumComponents>2) return fail();
    std::memcpy(comp.mFrameComp,raw+4,16);
    u32 offset=h.mCompInfoDataOffsets+20;
    bool video=false,audio=false;
    for(u32 i=0;i<comp.mNumComponents;++i) {
        if(comp.mFrameComp[i]==0 && !video) {
            if(!read(raw,12,offset)) return fail();
            ActivePlayer.mVideoInfo={be(raw),be(raw+4),be(raw+8)}; video=true; offset+=12;
        } else if(comp.mFrameComp[i]==1 && !audio) {
            if(!read(raw,16,offset)) return fail();
            ActivePlayer.mAudioInfo={be(raw),be(raw+4),be(raw+8),be(raw+12)}; audio=true; offset+=16;
        } else return fail();
    }
    const auto w=ActivePlayer.mVideoInfo.mXSize,hgt=ActivePlayer.mVideoInfo.mYSize;
    if(!video || !w || !hgt || w>1024 || hgt>1024 || (w%16) || (hgt%16)) return fail();
    yBytes=aligned(size_t(w)*hgt); cBytes=aligned(size_t(w/2)*(hgt/2));
    ActivePlayer.mAudioExist=audio;
    ActivePlayer.mCurVolume=ActivePlayer.mTargetVolume=127;
    return TRUE;
}
BOOL THPPlayerClose() {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || ActivePlayer.mState) return FALSE;
    close(); return TRUE;
}
u32 THPPlayerCalcNeedMemory() {
    std::lock_guard<std::mutex> lock(mutex);
    return ActivePlayer.mIsOpen?u32(31+aligned(ActivePlayer.mHeader.mBufferSize)+yBytes+2*cBytes):0;
}
BOOL THPPlayerSetBuffer(u8* data) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || ActivePlayer.mState || !data) return FALSE;
    compressed=reinterpret_cast<u8*>((reinterpret_cast<std::uintptr_t>(data)+31)&~std::uintptr_t(31));
    auto& texture=ActivePlayer.mTextureSet[0];
    texture.mYTexture=compressed+aligned(ActivePlayer.mHeader.mBufferSize);
    texture.mUTexture=texture.mYTexture+yBytes; texture.mVTexture=texture.mUTexture+cBytes;
    return TRUE;
}
BOOL THPPlayerPrepare(int frame,u8 flag,int track) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || ActivePlayer.mState || !compressed || frame<0 ||
       u32(frame)>=ActivePlayer.mHeader.mNumFrames || track<0 ||
       (ActivePlayer.mAudioExist && u32(track)>=ActivePlayer.mAudioInfo.mSndNumTracks)) return FALSE;
    cursor=ActivePlayer.mHeader.mMovieDataOffsets; cursorSize=ActivePlayer.mHeader.mFirstFrameSize; cursorFrame=0;
    ActivePlayer.mInitReadFrame=frame; ActivePlayer.mPlayFlag=flag&1;
    ActivePlayer.mCurAudioTrack=track; ActivePlayer.mVideoDecodeCount=0;
    if(!decode(frame)) return FALSE;
    if(ActivePlayer.mAudioExist) resetAudio(u32(frame));
    elapsed=0; ActivePlayer.mState=1; return TRUE;
}
BOOL THPPlayerPlay() {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || (ActivePlayer.mState!=1 && ActivePlayer.mState!=4)) return FALSE;
    started=Clock::now(); if(retraceClock) startedRetrace=retraceClock(); ActivePlayer.mState=2; return TRUE;
}
BOOL THPPlayerPause() {
    std::lock_guard<std::mutex> lock(mutex);
    if(ActivePlayer.mState!=2) return FALSE;
    elapsed=position(); ActivePlayer.mState=4; return TRUE;
}
void THPPlayerStop() { std::lock_guard<std::mutex> lock(mutex); stop(); }
BOOL THPPlayerSetVolume(int vol,int duration) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || vol<0 || vol>127 || duration<0) return FALSE;
    // Silent builds keep this at 0 (the game passes 0); the real-audio mixer
    // applies it. Fades are applied immediately.
    ActivePlayer.mCurVolume=ActivePlayer.mTargetVolume=f32(vol); return TRUE;
}
BOOL THPPlayerGetVideoInfo(void* dst) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || !dst) return FALSE;
    std::memcpy(dst,&ActivePlayer.mVideoInfo,sizeof(THPVideoInfo)); return TRUE;
}
BOOL THPPlayerGetAudioInfo(void* dst) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || !dst) return FALSE;
    std::memcpy(dst,&ActivePlayer.mAudioInfo,sizeof(THPAudioInfo)); return TRUE;
}
u32 THPPlayerGetTotalFrame() { std::lock_guard<std::mutex> lock(mutex); return ActivePlayer.mIsOpen?ActivePlayer.mHeader.mNumFrames:0; }
u8 THPPlayerGetState() { std::lock_guard<std::mutex> lock(mutex); return ActivePlayer.mState; }
int THPPlayerDrawCurrentFrame(GXRenderModeObj* mode,int x,int y,int width,int height) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || !ActivePlayer.mState || !mode) return -1;
    if(ActivePlayer.mState==2) {
        const auto count=ActivePlayer.mHeader.mNumFrames;
        auto frame=std::uint64_t(position()*ActivePlayer.mHeader.mFrameRate)+ActivePlayer.mInitReadFrame;
        if(ActivePlayer.mPlayFlag) frame%=count;
        else if(frame>=count) { frame=count-1; ActivePlayer.mState=3; }
        if(!ActivePlayer.mDispTextureSet || frame!=u32(ActivePlayer.mDispTextureSet->mFrameNumber)) {
            if(!decode(u32(frame))) OSPanic(__FILE__,__LINE__,"Native THP frame decode failed (DVD %d, video %d)",ActivePlayer.mDvdError,ActivePlayer.mVideoError);
        }
    }
    auto* tex=ActivePlayer.mDispTextureSet;
    if(!tex) return -1;
    THPGXYuv2RgbSetup(mode);
    THPGXYuv2RgbDraw(reinterpret_cast<u32*>(tex->mYTexture),reinterpret_cast<u32*>(tex->mUTexture),reinterpret_cast<u32*>(tex->mVTexture),
        x,y,ActivePlayer.mVideoInfo.mXSize,ActivePlayer.mVideoInfo.mYSize,width,height);
    THPGXRestore(); submitted=true;
    return tex->mFrameNumber;
}
void p2_thp_use_retrace_clock(u32 (*retraces)()) { std::lock_guard<std::mutex> lock(mutex); retraceClock=retraces; }
// Adds `frames` stereo samples at `rate` Hz of movie audio into `stereo`
// (interleaved float). Called by the real-audio mixer; silent builds never call it.
void p2_thp_audio_mix(float* stereo,u32 frames,u32 rate) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ActivePlayer.mIsOpen || ActivePlayer.mState!=2 || !ActivePlayer.mAudioExist || !rate) return;
    const double step=double(ActivePlayer.mAudioInfo.mSndFrequency)/rate;
    const float volume=ActivePlayer.mCurVolume/127.0f;
    for(u32 i=0;i<frames;++i) {
        while(audioRead+2>=audioQueue.size() - (audioQueue.size()%2)) {
            P2HostScratchScope hostStorage;
            if(audioRead) { audioQueue.erase(audioQueue.begin(),audioQueue.begin()+audioRead); audioRead=0; }
            if(!decodeAudioFrame()) return;
        }
        const float t=float(audioPhase);
        const s16* a=&audioQueue[audioRead]; const s16* b=a+2;
        // THPAudioDecode interleaves the right channel first.
        stereo[i*2]+=volume*((a[1]+(b[1]-a[1])*t)/32768.0f);
        stereo[i*2+1]+=volume*((a[0]+(b[0]-a[0])*t)/32768.0f);
        for(audioPhase+=step;audioPhase>=1.0;audioPhase-=1.0) audioRead+=2;
    }
}
void THPPlayerDrawDone() { std::lock_guard<std::mutex> lock(mutex); finishDraw(); }
void THPPlayerPostDrawDone() { THPPlayerDrawDone(); }
}
