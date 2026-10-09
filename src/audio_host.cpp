// Host audio for real JAudio (p2_audio_bringup).
//
// The console mixed JASDsp::TChannel voices on the DSP and played the result
// by AI DMA, with JASAudioThread woken by both interrupts. Here the audio
// thread renders directly: when the SDL output queue runs low it runs one
// DAC frame (getSubFrames() subframes of updateDSP + a native mix of the
// channel table), the per-frame DAC callbacks, and queues the samples.
//
// The voice mixer is adapted from Dusklight's DuskDsp.cpp (CC0, Twilight
// Princess, same DSP channel layout; see the offset checks below): ADPCM4 and
// PCM16 decode from ARAM, linear resampling, the DSP's one-pole and biquad
// IIR filters, auto-mixer pan/volume, and a shared reverb on the FX send. Not
// yet: the DSP's own FX-line algorithm, oscillator 11 (approximated), ADPCM2/PCM8.
#include "Dolphin/os.h"
#include "JSystem/JAudio/JAS/JASAudioThread.h"
#include "JSystem/JAudio/JAS/JASDriver.h"
#include "JSystem/JAudio/JAS/JASDsp.h"
#include "p2_aram.h"
#include "p2_endian.h"
#include "p2_game_alloc.h"
#include "p2_memory.h"
#include "p2_renderer.h"
#include <SDL3/SDL.h>
#include <freeverb/revmodel.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

using JASDsp::TChannel;

// Same ucode family as Twilight Princess: Dusklight's field meanings apply at
// these offsets (Pikmin 2 decomp names in brackets where they differ).
static_assert(sizeof(TChannel) == 0x180, "DSP channel stride");
static_assert(offsetof(TChannel, mPitch) == 0x04, "pitch");
static_assert(offsetof(TChannel, mIsPlaying) == 0x08, "reset request [mIsPlaying]");
static_assert(offsetof(TChannel, mPauseFlag) == 0x0C, "pause");
static_assert(offsetof(TChannel, mVolumeAndPan) == 0x50, "auto-mixer pan<<8|dolby [mVolumeAndPan]");
static_assert(offsetof(TChannel, mFxMixAndDolby) == 0x52, "auto-mixer fx [mFxMixAndDolby]");
static_assert(offsetof(TChannel, mCurrentMixerValue) == 0x54, "auto-mixer start volume");
static_assert(offsetof(TChannel, mMixerLevel) == 0x56, "auto-mixer volume");
static_assert(offsetof(TChannel, mIsMixerInitialized) == 0x58, "auto-mixer set");
static_assert(offsetof(TChannel, mSamplesPerBlock) == 0x64, "samples per block");
static_assert(offsetof(TChannel, mBlockCount) == 0x68, "sample position [mBlockCount]");
static_assert(offsetof(TChannel, mSampleOffset) == 0x70, "ARAM stream position [mSampleOffset]");
static_assert(offsetof(TChannel, mCurrentSampleOffset) == 0x74, "samples left [mCurrentSampleOffset]");
static_assert(offsetof(TChannel, mBytesPerBlock) == 0x100, "bytes per block");
static_assert(offsetof(TChannel, mLoopOffset) == 0x102, "loop flag [mLoopOffset]");
static_assert(offsetof(TChannel, mLast) == 0x104 && offsetof(TChannel, mPenult) == 0x106, "loop history");
static_assert(offsetof(TChannel, mFilterMode) == 0x108 && offsetof(TChannel, mForcedStop) == 0x10A, "mode/stop");
static_assert(offsetof(TChannel, mLoopStartOffset) == 0x110, "loop start sample");
static_assert(offsetof(TChannel, mNextSampleOffset) == 0x114, "end sample [mNextSampleOffset]");
static_assert(offsetof(TChannel, mDataOffset) == 0x118, "wave ARAM address [mDataOffset]");
static_assert(offsetof(TChannel, mIirFilterParam) == 0x148 && offsetof(TChannel, mDistFilter) == 0x150, "IIR [4] + low-pass");

namespace {
constexpr int SampleRate = 32000, SubframeSize = 80, Channels = 64;
using Subframe = std::array<float, SubframeSize>;

struct Voice {
    s16 hist1, hist0;
    static constexpr int DecodeSize = 2048;
    s16 decode[DecodeSize];
    int decodeCount;
    float resamplePos;
    s16 resamplePrev;
    u16 oscPhase;
    float lpIn, lpOut, bIn1, bIn2, bOut1, bOut2;
    float prevLeft, prevRight;
    bool havePrev;
};
Voice voices[Channels];
bool trace; // P2_AUDIO_TRACE

constexpr s16 Coef0[16] = {0, 0x0800, 0, 0x0400, 0x1000, 0x0e00, 0x0c00, 0x1200,
                           0x1068, 0x12c0, 0x1400, 0x0800, 0x0400, s16(0xfc00), s16(0xfc00), s16(0xf800)};
constexpr s16 Coef1[16] = {0, 0, 0x0800, 0x0400, s16(0xf800), s16(0xfa00), s16(0xfc00), s16(0xf600),
                           s16(0xf738), s16(0xf704), s16(0xf400), s16(0xf800), s16(0xfc00), 0x0400, 0, 0};
s16 clamp16(s32 v) { return s16(std::clamp(v, -0x8000, 0x7FFF)); }

void decodeAdpcm4(const u8* in, size_t frames, s16* out, size_t samples, s16& hist2, s16& hist1) {
    s16* end = out + samples;
    for (size_t f = 0; f < frames; ++f, in += 9) {
        const s32 scale = 1 << (in[0] >> 4);
        const s16 c0 = Coef0[in[0] & 15], c1 = Coef1[in[0] & 15];
        for (int i = 0; i < 16; ++i) {
            const u8 byte = in[1 + i / 2];
            const s8 nibble = s8(u8((i & 1 ? byte & 15 : byte >> 4) << 4)) >> 4;
            const s16 sample = clamp16((((nibble * scale) << 11) + (c0 * hist1 + c1 * hist2)) >> 11);
            hist2 = hist1; hist1 = sample;
            *out++ = sample;
            if (out == end) return;
        }
    }
}

u32 blockBytes(const TChannel& c) { return c.mSamplesPerBlock == 1 ? (c.mBytesPerBlock == 16 ? 2 : 1) : c.mBytesPerBlock; }
u32 dataLength(const TChannel& c, u32 samples) {
    if (samples % c.mSamplesPerBlock) samples += c.mSamplesPerBlock;
    return samples / c.mSamplesPerBlock * blockBytes(c);
}
bool supported(const TChannel& c) {
    return (c.mSamplesPerBlock == 16 && c.mBytesPerBlock == 9) || (c.mSamplesPerBlock == 1 && c.mBytesPerBlock == 16);
}

void reset(TChannel& c, Voice& v) {
    c.mCurrentSampleOffset = c.mNextSampleOffset - c.mBlockCount;
    v = Voice{};
    c.mIsPlaying = 0;
}

constexpr int DecodeSizeLimit = Voice::DecodeSize + 32;
int readChunk(TChannel& c, Voice& v, int wanted, s16* out, int outSize) {
    u32 position = c.mBlockCount;
    const u32 skip = position % c.mSamplesPerBlock;
    if (skip) { // loops can land mid-block: decode from the block start, drop the lead-in
        wanted += skip; position -= skip;
        c.mCurrentSampleOffset += skip; c.mBlockCount -= skip;
    }
    wanted = (wanted + c.mSamplesPerBlock - 1) / c.mSamplesPerBlock * c.mSamplesPerBlock;
    const u32 render = std::min<u32>(c.mCurrentSampleOffset, wanted);
    const u32 offset = dataLength(c, position), bytes = dataLength(c, render);
    const u8* data = p2_aram_host(c.mDataOffset + offset, bytes);
    s16 pcm[DecodeSizeLimit];
    if (render > sizeof(pcm) / sizeof(pcm[0]) || !data) {
        std::memset(out, 0, sizeof(s16) * std::min<int>(outSize, render));
        c.mCurrentSampleOffset = 0;
        return 0;
    }
    if (c.mSamplesPerBlock == 1)
        for (u32 i = 0; i < render; ++i) pcm[i] = p2_read_big<s16>(data + i * 2);
    else
        decodeAdpcm4(data, bytes / 9, pcm, render, v.hist1, v.hist0);
    c.mCurrentSampleOffset -= render;
    c.mBlockCount += render;
    const int count = std::min<int>(int(render) - int(skip), outSize);
    if (count > 0) std::memcpy(out, pcm + skip, count * sizeof(s16));
    return std::max(count, 0);
}
void fill(TChannel& c, Voice& v, int needed) {
    while (v.decodeCount < needed) {
        if (!c.mCurrentSampleOffset) {
            if (!c.mLoopOffset) break;
            c.mCurrentSampleOffset = c.mNextSampleOffset - c.mLoopStartOffset;
            c.mBlockCount = c.mLoopStartOffset;
            v.hist1 = c.mPenult; v.hist0 = c.mLast;
        }
        const int space = Voice::DecodeSize - v.decodeCount;
        if (!space) break;
        const int got = readChunk(c, v, std::min(space, needed - v.decodeCount), v.decode + v.decodeCount, space);
        if (!got && !c.mCurrentSampleOffset && !c.mLoopOffset) break;
        v.decodeCount += got;
    }
    c.mSampleOffset = c.mDataOffset + dataLength(c, c.mBlockCount);
}

void renderOsc(TChannel& c, Voice& v, Subframe& out) {
    if (c.mIsPlaying) reset(c, v);
    const u32 step = u16(c.mPitch) >> 1;
    for (float& s : out) {
        switch (c.mBytesPerBlock) {
        case 0: s = v.oscPhase < 0x8000 ? 0.5f : -0.5f; break;
        case 3: s = v.oscPhase < 0x4000 ? 0.5f : -0.5f; break;
        case 1: case 12: s = float(s16(v.oscPhase)) / 32768.0f; break;
        case 4: s = 0.5f - std::fabs(float(s16(v.oscPhase)) / 32768.0f); break;
        default: s = std::sin(float(v.oscPhase) * (6.2831853f / 65536.0f)) * 0.5f; break;
        }
        v.oscPhase = u16(v.oscPhase + step);
    }
}

void renderWave(TChannel& c, Voice& v, Subframe& out) {
    if (c.mIsPlaying) reset(c, v);
    const float step = float(u16(c.mPitch)) / 4096.0f;
    const int needed = int(v.resamplePos + SubframeSize * step) + 2;
    fill(c, v, needed);
    if (v.decodeCount < needed) c.mIsFinished = true;
    float pos = v.resamplePos;
    s16 prev = v.resamplePrev, next = v.decodeCount > 0 ? v.decode[0] : prev;
    int index = 0;
    for (float& s : out) {
        s = (prev + pos * (next - prev)) / 32768.0f;
        for (pos += step; pos >= 1.0f; pos -= 1.0f) {
            prev = next;
            ++index;
            next = index < v.decodeCount ? v.decode[index] : prev;
        }
    }
    v.resamplePos = pos; v.resamplePrev = prev;
    if (const s16 k = c.mDistFilter) // one-pole low-pass: out = (in - in1) * k/128 + out1
        for (float& s : out) {
            const float o = std::clamp((s - v.lpIn) * (k / 128.0f) + v.lpOut, -1.0f, 1.0f);
            v.lpIn = s; s = v.lpOut = o;
        }
    if (c.mFilterMode & 0x20)
        for (float& s : out) {
            const s16* p = c.mIirFilterParam;
            const float o = std::clamp((p[0] * v.bIn1 + p[1] * v.bIn2 + p[2] * v.bOut1 + p[3] * v.bOut2) / 32768.0f, -1.0f, 1.0f);
            v.bIn2 = v.bIn1; v.bIn1 = s; v.bOut2 = v.bOut1; s = v.bOut1 = o;
        }
    const int left = v.decodeCount - index;
    if (left > 0) std::memmove(v.decode, v.decode + index, left * sizeof(s16));
    v.decodeCount = std::max(0, left);
}

float volumeFromU16(u16 value) { return float(value) / 32767.0f; }

// FX send. The DSP ran Pikmin 2's FX lines (delay + 8-tap filter per scene);
// this is Dusklight's shared stereo reverb fed by each voice's auto-mixer FX
// level instead. P2_AUDIO_REVERB=0 disables it.
revmodel reverb;
bool reverbEnabled = true, reverbHasTail;
float reverbOutputEnergy; u32 reverbSendVoices; // trace counters
void initReverb() {
    reverb.setwet(1.0f); reverb.setdry(0.0f); reverb.setroomsize(0.5f);
    reverb.setdamp(0.7f); reverb.setwidth(1.0f); reverb.setmode(0.0f); reverb.mute();
    if (const char* value = std::getenv("P2_AUDIO_REVERB")) reverbEnabled = std::strcmp(value, "0") != 0;
}

// Renders one subframe of all active voices into interleaved stereo.
void mix(float* stereo) {
    std::fill(stereo, stereo + SubframeSize * 2, 0.0f);
    TChannel* table = JASDsp::CH_BUF;
    if (!table) return;
    float sendLeft[SubframeSize] = {}, sendRight[SubframeSize] = {};
    bool anySend = false;
    for (int i = 0; i < Channels; ++i) {
        TChannel& c = table[i];
        Voice& v = voices[i];
        if (!c.mIsActive || c.mPauseFlag) continue;
        if (c.mForcedStop) { c.mIsFinished = true; continue; }
        if (trace && c.mIsPlaying) {
            const u8* head = c.mDataOffset ? p2_aram_host(c.mDataOffset, 16) : nullptr;
            std::fprintf(stderr, "[AUDIO] voice %d start: %u samples/%u bytes per block, ARAM 0x%x (%s), pos %u end %u loop %u@%u, "
                "pitch 0x%x, auto %u level %u pan/dolby 0x%x, mix[0] bus 0x%x vol %d\n", i, c.mSamplesPerBlock, c.mBytesPerBlock,
                c.mDataOffset, head ? "mapped" : "unmapped", c.mBlockCount, c.mNextSampleOffset, c.mLoopOffset, c.mLoopStartOffset,
                u16(c.mPitch), c.mIsMixerInitialized, c.mMixerLevel, c.mVolumeAndPan, c.mMixer[0].mBusConnect, c.mMixer[0].mMixVolume);
            if (head) std::fprintf(stderr, "[AUDIO]   data %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", head[0], head[1], head[2],
                head[3], head[4], head[5], head[6], head[7], head[8]);
        }
        Subframe mono{};
        if (!c.mDataOffset) renderOsc(c, v, mono);
        else if (supported(c)) renderWave(c, v, mono);
        else { c.mIsFinished = true; continue; }
        float left = 0, right = 0, initLeft = 0, initRight = 0;
        if (c.mIsMixerInitialized) {
            const float volume = volumeFromU16(c.mMixerLevel), init = volumeFromU16(c.mCurrentMixerValue);
            const float pan = float(c.mVolumeAndPan >> 8) / 127.0f;
            left = (1 - pan) * volume; right = pan * volume;
            initLeft = (1 - pan) * init; initRight = pan * init;
        } else {
            for (const auto& m : c.mMixer) {
                if (m.mBusConnect == 0x0D00) { left = volumeFromU16(m.mMixVolume); initLeft = volumeFromU16(m.mBaseVolume); }
                if (m.mBusConnect == 0x0D60) { right = volumeFromU16(m.mMixVolume); initRight = volumeFromU16(m.mBaseVolume); }
            }
        }
        if (!v.havePrev) { v.prevLeft = initLeft; v.prevRight = initRight; v.havePrev = true; }
        // Scaled send rather than wet/dry on the output, so a tail decays at the
        // level it was fed with (Dusklight's 600 divisor, tuned by ear there).
        const float send = (reverbEnabled && c.mIsMixerInitialized) ? (c.mFxMixAndDolby >> 8) / 600.0f : 0.0f;
        anySend |= send > 0;
        reverbSendVoices += send > 0;
        for (int s = 0; s < SubframeSize; ++s) {
            const float t = float(s) / SubframeSize;
            const float l = mono[s] * (v.prevLeft + (left - v.prevLeft) * t);
            const float r = mono[s] * (v.prevRight + (right - v.prevRight) * t);
            stereo[s * 2] += l;
            stereo[s * 2 + 1] += r;
            if (send > 0) { sendLeft[s] += l * send; sendRight[s] += r * send; }
        }
        v.prevLeft = left; v.prevRight = right;
    }
    if (reverbEnabled && (anySend || reverbHasTail)) {
        float wetLeft[SubframeSize] = {}, wetRight[SubframeSize] = {};
        const float energy = reverb.processreplace(sendLeft, sendRight, wetLeft, wetRight, SubframeSize, 1, 1.0f);
        for (int s = 0; s < SubframeSize; ++s) { stereo[s * 2] += wetLeft[s]; stereo[s * 2 + 1] += wetRight[s]; }
        reverbHasTail = energy >= 2.0f * SubframeSize * 1e-8f; // about -80 dBFS
        reverbOutputEnergy += energy;
    }
}

SDL_AudioStream* output;
std::atomic<bool> stopRequested, stopped;

// P2_AUDIO_DUMP=<file.wav> also writes everything rendered (32 kHz stereo
// float). With SDL_AUDIO_DRIVER=dummy, tests measure audio without playing it.
FILE* dump;
u32 dumpBytes;
void writeDumpHeader() {
    const u32 riff = 36 + dumpBytes, fmtSize = 16, rate = SampleRate, byteRate = SampleRate * 8;
    const u16 format = 3, channels = 2, align = 8, bits = 32;
    std::fseek(dump, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, dump); std::fwrite(&riff, 4, 1, dump); std::fwrite("WAVEfmt ", 1, 8, dump);
    std::fwrite(&fmtSize, 4, 1, dump); std::fwrite(&format, 2, 1, dump); std::fwrite(&channels, 2, 1, dump);
    std::fwrite(&rate, 4, 1, dump); std::fwrite(&byteRate, 4, 1, dump); std::fwrite(&align, 2, 1, dump);
    std::fwrite(&bits, 2, 1, dump); std::fwrite("data", 1, 4, dump); std::fwrite(&dumpBytes, 4, 1, dump);
    std::fseek(dump, 0, SEEK_END);
}
}

extern "C" void p2_audio_host_shutdown();
extern "C" void p2_thp_audio_mix(float* stereo, u32 frames, u32 rate); // movie audio (thp_player.cpp)

// The audio thread's render loop (replaces its AI/DSP message loop).
extern "C" void p2_audio_host_run() {
    // The game replaces global operator new process-wide and this thread has a
    // JKR heap selected, so SDL and CoreAudio would allocate from it (and fail
    // inside CoreAudio's own setup). Every SDL call here uses host memory.
    std::vector<float> frame;
    u32 subframes = 0;
    {
        P2HostScratchScope hostStorage;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "*** audio: SDL audio init failed: %s\n", SDL_GetError());
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, SampleRate};
        output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!output) {
            std::fprintf(stderr, "*** audio: no output device: %s\n", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(output);
        {
            const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(output);
            SDL_AudioSpec deviceSpec{};
            int deviceFrames = 0;
            SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames);
            const char* name = SDL_GetAudioDeviceName(device);
            std::fprintf(stderr, "[AUDIO] output: %s (%s driver), %d Hz, %d channels, %d-frame buffer\n", name ? name : "unknown",
                SDL_GetCurrentAudioDriver(), deviceSpec.freq, deviceSpec.channels, deviceFrames);
        }
        p2_renderer_set_shutdown_hook(p2_audio_host_shutdown);
        if (const char* path = std::getenv("P2_AUDIO_DUMP")) {
            dump = std::fopen(path, "wb");
            if (dump) writeDumpHeader();
        }
        subframes = JASDriver::getSubFrames();
        frame.resize(subframes * SubframeSize * 2);
    }
    const int frameBytes = int(frame.size() * sizeof(float));
    p2_thread_exclude_from_barrier(); // renders continuously in real time
    trace = std::getenv("P2_AUDIO_TRACE") != nullptr;
    initReverb();
    u32 frames = 0;
    while (!stopRequested) {
        // Keep about three frames (~50 ms) queued: the DMA interrupt's role.
        int queued;
        {
            P2HostScratchScope hostStorage;
            queued = SDL_GetAudioStreamQueued(output);
        }
        if (queued > frameBytes * 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const BOOL level = OSDisableInterrupts();
        JASAudioThread::snIntCount = subframes;
        for (u32 s = 0; s < subframes; ++s) {
            JASDriver::updateDSP();
            mix(frame.data() + s * SubframeSize * 2);
            JASAudioThread::snIntCount = JASAudioThread::snIntCount - 1;
        }
        JASDriver::updateDacCallback();
        p2_thp_audio_mix(frame.data(), subframes * SubframeSize, SampleRate);
        if (trace && ++frames % 57 == 0) { // about once a second
            int active = 0, audible = 0;
            for (int i = 0; i < Channels; ++i)
                if (JASDsp::CH_BUF && JASDsp::CH_BUF[i].mIsActive) {
                    ++active;
                    audible += JASDsp::CH_BUF[i].mMixerLevel != 0 || JASDsp::CH_BUF[i].mMixer[0].mMixVolume != 0;
                }
            std::fprintf(stderr, "[AUDIO] frame %u: %d active voices, %d with volume; reverb send voice-subframes %u, wet energy %.4f\n",
                frames, active, audible, reverbSendVoices, reverbOutputEnergy);
            reverbSendVoices = 0; reverbOutputEnergy = 0;
            for (int line = 0; JASDsp::FX_BUF && line < 4; ++line) {
                const JASDsp::Fxline& fx = JASDsp::FX_BUF[line];
                if (fx.mBufferStatus)
                    std::fprintf(stderr, "[AUDIO]   FX line %d: status %u, %u blocks, send 0x%x/0x%x, taps %d %d %d %d %d %d %d %d\n", line,
                        fx.mBufferStatus, fx._02, fx._08, fx._0C, s16(fx._10[0]), s16(fx._10[1]), s16(fx._10[2]), s16(fx._10[3]),
                        s16(fx._10[4]), s16(fx._10[5]), s16(fx._10[6]), s16(fx._10[7]));
            }
            for (int i = 0; i < Channels; ++i) {
                const TChannel& c = JASDsp::CH_BUF[i];
                if (c.mIsActive)
                    std::fprintf(stderr, "[AUDIO]   voice %d pitch 0x%x level %u start %u pan 0x%x pos %u left %u finished %u decoded %d\n", i,
                        u16(c.mPitch), c.mMixerLevel, c.mCurrentMixerValue, c.mVolumeAndPan, c.mBlockCount, c.mCurrentSampleOffset,
                        c.mIsFinished, voices[i].decodeCount);
            }
        }
        OSRestoreInterrupts(level);
        P2HostScratchScope hostStorage;
        SDL_PutAudioStreamData(output, frame.data(), frameBytes);
        if (dump) {
            std::fwrite(frame.data(), 1, frameBytes, dump);
            dumpBytes += frameBytes;
            writeDumpHeader(); // keep the file valid if the process exits mid-run
        }
    }
    stopped = true;
}

// Called by renderer shutdown before SDL is torn down.
extern "C" void p2_audio_host_shutdown() {
    if (!output) return;
    stopRequested = true;
    for (int i = 0; i < 200 && !stopped; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    P2HostScratchScope hostStorage;
    SDL_DestroyAudioStream(output);
    output = nullptr;
    if (dump) { std::fclose(dump); dump = nullptr; }
}

// Hardware entry points the original driver still calls. AI DMA and the DSP
// are replaced by the loop above.
extern "C" {
void AIInit(u8*) {}
u32 AIRegisterDMACallback(void (*)()) { return 0; }
void AIInitDMA(u32, u32) {}
void AIStartDMA() {}
void AIStopDMA() {}
void AISetDSPSampleRate(u32) {}
u32 DSPCheckMailFromDSP() { return 0; }
u32 DSPReadMailFromDSP() { return 0; }
}
void DspBoot(void (*)(void*)) {}
void DspFinishWork(u16) {}
void DSPReleaseHalt2(u32) {}
void DsetMixerLevel(f32) {}
void DsyncFrame2(u32, u32, u32) {}
