# Architecture

The organising idea is that **the game does not know it has been ported.**

The decompiled game code keeps its shape. What the GameCube used to provide —
the disc drive, ARAM, the memory card, the audio hardware, the GX graphics
pipeline — is reimplemented behind the same interfaces, so game code calls what
it always called and something native happens instead.

```
   the decompiled game  (vendor/pikmin2 -> prepared, patched at build time)
            │
            │  the Dolphin SDK surface: DVD, AR, CARD, OS, PAD, VI, GX
            ▼
   the port layer  (src/)              Aurora  (vendor/aurora)
            │                                │
            ▼                                ▼
   host filesystem · CoreAudio        Metal, via Dawn
```

## Two preparation steps, not a patch series

This is the part that differs from most ports. Nothing is vendored and nothing
is diffed:

- `tools/prepare_source.py` takes the pinned decompilation, copies `src` and
  `include` out of it with `git archive`, and applies this port's changes as
  **Python transformations** — `scope_switches.py` for case bodies MWCC
  accepted and Clang rejects, `jaudio_patches.py` for the 64-bit and
  byte-order corrections the audio library needs. Output goes to an ignored
  build directory. The reference checkout is never touched.
- `tools/prepare_renderer.py` does the same for Aurora.

Both are content-signed, so editing a transformation recompiles only what it
touched. Edit the scripts, never the generated tree — generated files are
overwritten without warning.

## The port layer

| | |
|---|---|
| `dvd.cpp`, `dvd_worker.cpp` | The disc. Parses the real `sys/fst.bin` so the game's own FST entry numbers resolve, and serves its synchronous and asynchronous file APIs from the extracted tree |
| `aram.cpp`, `aram_memory.cpp` | ARAM — the GameCube's auxiliary audio RAM, as a host allocation with the same transfer semantics |
| `card.cpp` | Memory-card emulation backed by host files |
| `audio_host.cpp` | The audio path, including the Freeverb-based FX send. `audio_silent.cpp` is the stub it replaced |
| `renderer_gx.cpp`, `gx_fifo.cpp`, `renderer_frame.cpp` | The GX seam: command submission, frame lifecycle, and the handoff to Aurora |
| `material.cpp`, `material2d.cpp`, `shape.cpp`, `skin.cpp`, `vertex.cpp`, `texture_refs.cpp` | Host-side J3D and J2D paths — the model, material and geometry handling that 64-bit pointers and little-endian layout break if left alone |
| `memory.cpp`, `game_alloc.cpp`, `host_scratch.cpp` | Allocation, including the game's own heap behaviour |
| `threads.cpp`, `critical.cpp`, `messages.cpp` | OS threads, mutexes and the message queues |
| `thp_player.cpp`, `video.cpp` | THP video playback |
| `input_record.cpp` | Input recording and replay, keyed to the simulation tick. The backbone of testing |
| `crash.cpp`, `panic.cpp` | Self-reporting crash output |
| `touch_pad.cpp`, `touch_draw.cpp` | Touch controls. Present but iOS-facing; not used by the macOS build |

## Two things worth knowing before changing anything

**Testing is replay, not unit tests.** `input_record.cpp` records controller
input against the simulation tick and replays it deterministically. That is how
a change is shown not to have broken anything. The `ctest` suite covers host
primitives — the heap, threading, streams, the card command layer — and does
not touch the game.

**The warning gates are load-bearing.** The build turns a set of bug-class
warnings into errors, with a recorded baseline in
`tools/warning_baseline.txt`. They are not style: each one corresponds to a
class of port bug that has actually occurred here, mostly 64-bit layout and
byte-order mistakes that a GameCube compiler never had to care about. Adding
`-Wno-` to silence one is almost always the wrong fix.

## Where the answers are

In the comments, and in the transformation scripts. Where behaviour is
non-obvious *because the hardware was strange*, the comment at that site is
usually the only place the knowledge exists.

For questions about the decompiled game code itself rather than the port,
[projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) is the place to
ask. For the renderer, [Aurora](https://github.com/encounter/aurora).
