# Credits

This port is a thin layer over several much larger efforts. Worth stating the
proportions: the decompilation is hundreds of thousands of lines of
reverse-engineered source, and Aurora is a complete GX implementation on top of
a modern graphics API. Neither was done here.

---

## The Pikmin 2 decompilation — [projectPiki/pikmin2](https://github.com/projectPiki/pikmin2)

Everything this project does rests on projectPiki's decompilation of *Pikmin 2*
(`GPVE01`). Reconstructing a GameCube game's source so that it recompiles to the
original binary is years of collective work, and it is what makes a port
possible rather than merely imaginable: when the source provably matches, a
divergence here is unambiguously this port's fault.

Published under CC0 1.0, which asks nothing of anyone using it — no
attribution, no notice, no conditions. This credit is given because it is
deserved, not because it is required.

Pinned at the commit in `upstream-revision.txt`. Questions about the
decompilation itself, and contributions to it, belong there rather than here.

Licence text: [`third-party/LICENSE-projectPiki-CC0.txt`](third-party/LICENSE-projectPiki-CC0.txt).

## Aurora — [encounter/aurora](https://github.com/encounter/aurora)

Luke Street's Aurora implements the GameCube's GX graphics API over a modern
backend, and supplies this port's Metal path, along with GD, VI, OS and PAD
layers. Reimplementing a fixed-function register-machine GPU accurately is the
single largest piece of work a GameCube port needs, and this project did not
have to do it.

MIT. Pinned at the commit in `aurora-revision.txt`, fetched by `setup.sh`.
Licence text: [`third-party/LICENSE-aurora-MIT.txt`](third-party/LICENSE-aurora-MIT.txt).

## Dusklight — [TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight)

Dusklight solved a great deal of what this port needed before this port
existed: driving Aurora from a JSystem-based GameCube game, the 64-bit
corrections that J3D model loading requires, and the Freeverb modifications
this project uses directly for its reverb send. CC0.

Licence text: [`third-party/LICENSE-dusklight-CC0.txt`](third-party/LICENSE-dusklight-CC0.txt).

## Freeverb — Jezar at Dreampoint

The public-domain reverb that every game-audio project eventually meets. Used
here through Dusklight's modified version.

## Aurora's dependencies

Dawn (Google, BSD-3-Clause) for WebGPU, SDL3 (zlib), xxHash (BSD-2-Clause),
Dear ImGui (MIT) and Tracy (BSD-3-Clause). Fetched by Aurora's build; none are
redistributed here.

## The GameCube reverse-engineering community

The port layer emulates hardware this project did not document: the DVD and
FST layout, ARAM, the memory card, the audio path, the THP video format. That
knowledge was worked out over two decades by people writing emulators,
homebrew SDKs and format notes — the Dolphin project foremost among them.
Rarely citable as a single source, and the reason a bug here can usually be
diagnosed rather than merely observed.

---

## And, unavoidably: Nintendo

*Pikmin 2* was made by Nintendo EAD. This project exists because that game is
worth the trouble of keeping runnable.

Nothing here is authorised by Nintendo, nothing here is affiliated with
Nintendo, and nothing here redistributes their work. See
[docs/LEGAL.md](docs/LEGAL.md).
