# Building on macOS

Target: a Mac with Apple Silicon. There is no Intel build — the port assumes
arm64 and Metal throughout, and nobody has tried otherwise.

## 1. Prerequisites

```bash
xcode-select --install
brew install cmake ninja sdl3
```

Python 3.12 or newer, which the source-preparation scripts need. CMake 3.24+.

## 2. Get the sources

```bash
git clone https://github.com/magikarp/pikpik-2.git && cd pikpik-2
./setup.sh
```

`setup.sh` fetches two upstream projects at pinned commits:

- the decompilation into `vendor/pikmin2` (`upstream-revision.txt`)
- Aurora into `vendor/aurora` (`aurora-revision.txt`)

It fetches single commits rather than full histories, so it is quick. Neither
is modified by the build: `tools/prepare_source.py` and
`tools/prepare_renderer.py` copy the pinned revisions into ignored build
directories and apply this port's changes there.

`vendor/` is gitignored and must stay that way — see [LEGAL.md](LEGAL.md).

Re-runnable at any time. `./setup.sh --check` verifies without touching
anything; `./setup.sh --force` throws it away and refetches.

## 3. Game data

Not included. See [GAME_DATA.md](GAME_DATA.md). Short version: put the
extracted disc at `disc/` in the repository root, or point
`P2_DISC_DIRECTORY` at it.

## 4. Build

```bash
./build_mac.sh
```

**The first build is long.** Aurora fetches and compiles its own dependencies,
including Dawn, Google's WebGPU implementation. Budget an hour and leave it
alone. Later builds are incremental and take seconds to minutes.

Useful variations:

```bash
./build_mac.sh -c                      # wipe the build directory first
BUILD_TYPE=Debug ./build_mac.sh        # default is RelWithDebInfo
./build_mac.sh -DP2_UBSAN=ON           # UBSan, for chasing a suspected port bug
DISC_DIR=/path/to/disc ./build_mac.sh  # compile in a different disc path
```

The app lands at `build/pikpik-2.app`.

## 5. Run

```bash
open build/pikpik-2.app
```

Or from a terminal, which is what you want while debugging, because the
engine's logging goes to stdout:

```bash
./build/pikpik-2.app/Contents/MacOS/pikpik-2
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Host checks covering the heap, threading, streams, messages, the memory-card
command layer and asset parsing. They do not render, and they do not need the
disc — except the asset checks, which do.

## When it goes wrong

**`Aurora not found`** — `setup.sh` has not run, or `vendor/` was deleted. Run
it. Pass `-DP2_AURORA_SOURCE=/path/to/aurora` if you keep a checkout elsewhere.

**`vendor/ is missing`** from `build_mac.sh` — same cause.

**A dependency fails to download** during the first build — that is Aurora's
fetch, not this project's. Report the exact filename and error rather than
treating the port as blocked; these are usually transient network failures and
succeed on a re-run.

**`DVD mount requires GPVE01`** — the disc at `disc/` is not Pikmin 2 USA, or
it is an image rather than an extracted tree. See [GAME_DATA.md](GAME_DATA.md).

**It starts, then exits** — almost always the disc. Run from a terminal and
read the first lines of output; they say which path was tried.

**Something misbehaves only in an optimised build** — plausible. This is a
decompiled codebase written for a 1990s compiler, and optimisation exposes
latent undefined behaviour. Reproduce with `BUILD_TYPE=Debug` before assuming
your change is at fault, and see `-DP2_UBSAN=ON`.
