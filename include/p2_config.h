#pragma once

// Default builds are silent: no audio device, DSP, audio resource loading or
// audio worker. The real-audio app (p2_audio_bringup) compiles its own copy of
// the game code with P2_AUDIO_ENABLED=1.
#ifndef P2_AUDIO_ENABLED
#define P2_AUDIO_ENABLED 0
#endif

#if P2_AUDIO_ENABLED
#define P2_AUDIO_ONLY(...) do { __VA_ARGS__; } while (0)
#else
#define P2_AUDIO_ONLY(...) do { } while (0)
#endif
