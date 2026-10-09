# pikpik-2

A native **macOS (Metal)** source port of *Pikmin 2* for the Nintendo GameCube,
built on the [projectPiki decompilation](https://github.com/projectPiki/pikmin2)
and [Aurora](https://github.com/encounter/aurora).

Status: **in development.** Story mode runs — the title, menus, caves and
gameplay are reachable, with native audio, and roughly the first half of the
game has been played and debugged end to end. The rest is still being worked
through. Expect bugs, and expect them to be in the later game.

![Day 13 — red, blue and yellow Pikmin carrying a defeated bulborb, running
natively on macOS](https://raw.githubusercontent.com/magikarp/assets/main/screenshot2.jpg)

> **This repository contains no game code and no game assets.**
> It is the port layer. `setup.sh` fetches the decompiled sources and Aurora
> from their own upstream projects at pinned commits, and you supply the game
> data yourself from a disc you own. Read [docs/LEGAL.md](docs/LEGAL.md) first.

---

## Credit first

Very little of the hard part was done here. projectPiki decompiled the game;
Luke Street's Aurora implements the GameCube's GX graphics API over a modern
backend; Dusklight worked out much of how to drive Aurora from a JSystem game.
See [CREDITS.md](CREDITS.md).

## What you need

| | |
|---|---|
| Hardware | A Mac with Apple Silicon |
| Tools | Xcode command-line tools, CMake 3.24+, Ninja, Python 3.12+ |
| Libraries | `brew install cmake ninja sdl3` |
| Game data | An extracted `GPVE01` disc filesystem — *Pikmin 2*, USA. Not provided. See [docs/GAME_DATA.md](docs/GAME_DATA.md) |

## Build it

```bash
git clone https://github.com/magikarp/pikpik-2.git && cd pikpik-2
./setup.sh
```

`setup.sh` fetches the decompilation into `vendor/pikmin2` and Aurora into
`vendor/aurora`, both at pinned commits. Then put your extracted disc at
`disc/` in the repository root and build:

```bash
./build_mac.sh && open build/pikpik-2.app
```

The first build also fetches and compiles Aurora's own dependencies, Dawn
among them. Budget an hour for it. Later builds are incremental.

Full detail, including what to do when it goes wrong:
[docs/BUILDING_MACOS.md](docs/BUILDING_MACOS.md).

## iOS

Not in this release. The engine has been built and run on an iPad, but it
needs Dawn compiled from source with a patch, and that path is not yet
packaged for anyone else to follow. It will come.

## How it is put together

```
src/ include/      the port layer — everything that replaces GameCube hardware
tools/             source preparation: the pinned decomp in, a buildable tree out
cmake/             the build, including the Aurora integration
vendor/pikmin2/    the decompilation — fetched by setup.sh, never committed
vendor/aurora/     the GX/Metal layer — likewise
disc/              your extracted game data — likewise never committed
```

The game code keeps its shape. What the GameCube used to provide — the disc,
the filesystem, ARAM, the memory card, the audio hardware — is reimplemented in
`src/`, and GX goes to Aurora. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is
the tour.

## Documentation

| | |
|---|---|
| [docs/LEGAL.md](docs/LEGAL.md) | What is and is not distributed, and why. Read this one |
| [CREDITS.md](CREDITS.md) | Whose work this is built on |
| [docs/GAME_DATA.md](docs/GAME_DATA.md) | What the game data is and where it goes |
| [docs/BUILDING_MACOS.md](docs/BUILDING_MACOS.md) | The build, in full |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the port layer is organised |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Rules for contributions, including some hard ones |

## Licence

MIT for everything in this repository — see [LICENSE](LICENSE), and
[NOTICE](NOTICE) for the provenance of every component of a working build.

*PIKMIN* and *NINTENDO* are trademarks of Nintendo. This project is not
affiliated with, endorsed by, or approved by Nintendo.
