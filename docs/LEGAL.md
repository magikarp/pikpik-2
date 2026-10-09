# Legal position

What this project is, what it distributes, and where the genuine uncertainty
lies. Written to be accurate rather than reassuring. Nothing here is legal
advice, and the authors are not lawyers.

---

## 1. In one paragraph

pikpik-2 is a host port layer. It contains no game code and no game assets. To
produce a running program it needs three things this repository does not
provide: the *Pikmin 2* decompilation and Aurora, both of which `setup.sh`
fetches from their own upstream projects at pinned commits, and an extracted
copy of the retail GameCube disc, which you supply from a disc you own.
Everything in this repository — the port layer, the build, the
source-preparation scripts, the documentation — is original work by the
pikpik-2 authors, licensed MIT.

## 2. This is not a clean-room reimplementation

The term gets used loosely, and using it wrongly here would be a
misrepresentation.

A clean-room reimplementation separates two teams: one examines the original
and writes a functional specification containing no expressive content; the
other, having never seen the original, implements from that specification
alone. The firewall is the point — it makes the second team's output provably
not a copy.

**No such firewall exists here.** The game logic comes from a *decompilation*:
source produced by analysing the original compiled binary and iterating until a
recompile reproduces it. That is the opposite process. A decompilation is, by
construction and by intent, a representation of the original program.

What this project contributes is independent work — the disc and DVD services,
ARAM, the memory card, the audio host, the host-side J3D and J2D corrections,
the Aurora integration — but the game it drives is decompiled, and this project
does not claim otherwise.

## 3. On the decompilation's CC0 declaration

[projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) publishes under
CC0 1.0. That declaration is made in good faith and is meaningful as far as it
goes: it waives the upstream contributors' rights in their own contribution —
the naming, the structuring, the years of analysis.

It does not, and as a matter of law cannot, license the underlying work. Nobody
can place into the public domain a copyright they do not hold, and a
decompilation reproduces the structure, sequence and organisation of code whose
copyright belongs to its original author. A CC0 grant from a decompilation
project is best read as "we assert no rights against you", not as "this
material is unencumbered".

Decide for yourself what that means for you. This project's response to the
uncertainty is section 4.

## 4. Why nothing is vendored

This repository could have carried the decompiled sources, as several
comparable projects do. It deliberately does not, and the arrangement here goes
further than a patch series would.

`setup.sh` fetches the decompilation at a pinned commit. `tools/prepare_source.py`
then copies that pinned revision into an ignored build directory and applies
this port's changes there, never touching the reference checkout.

Those changes are expressed as **Python transformations, not diffs**. The
practical consequence matters: a unified diff carries context lines from the
file it patches, so a patch series contains fragments of the material it
patches. A transformation script does not. This repository therefore contains
no excerpt of the decompiled sources at all — not whole files, not fragments,
not context.

That is not a claim that the arrangement is bulletproof. It is that the smaller
the surface, the less there is to argue about.

## 5. Game assets

None are included, and none ever will be. The build reads an extracted disc
filesystem at a path you configure; without it the program starts, fails to
mount the disc, and stops.

Supply it yourself, from your own disc. Whether ripping a disc you own is
lawful depends on where you live: some jurisdictions permit a personal backup,
others prohibit circumventing the copy protection required to make one, and
some manage both at once. That is your determination, not this project's, and
this repository provides no tool and no instruction for obtaining game data by
any means.

That extends to screenshots. Any image in the README depicts Nintendo's artwork
rendered by this port, so it is hosted in a separate repository and embedded by
URL rather than committed here. A fine distinction, drawn deliberately: it
keeps section 2 of NOTICE literally true, and a request to take an image down
is satisfied somewhere else entirely.

### What the port actually reads

Narrower than "the disc", and worth stating precisely.

The port reads the game's **content**, plus two of the disc's structural files:

- `sys/boot.bin` — read only far enough to confirm the disc is `GPVE01` and
  reject anything else.
- `sys/fst.bin` — the filesystem table, parsed to resolve the game's own file
  entry numbers, because the game addresses its files by FST index.
- `files/` — the content the game loads.

It does not read, load or execute code from that data:

- `sys/main.dol`, the game's executable, is never opened by the port. The only
  code that runs is built from the decompilation and from this repository.
- The apploader is never opened.
- No GameCube DSP microcode is loaded or executed; the audio path is native.

Checkable in `src/dvd.cpp` and `src/game_main.cpp` rather than taken on trust.
`tools/check_dump.py` does read `sys/main.dol`, to verify your dump's SHA-1
against the pinned revision — that is a verification tool you run deliberately,
not part of the game.

A narrow claim, and it should not be read as a broad one. The content files are
still Nintendo's copyrighted work — textures, models, audio, level data are
theirs, you supply them, and nothing here changes that. What it means is that
no Nintendo program is executed at any point, natively or in emulation.

## 6. Trademark, and why the app is not called "Pikmin 2"

PIKMIN and NINTENDO are Nintendo's trademarks. This project is not affiliated
with or endorsed by Nintendo.

The documentation says "Pikmin 2" where it must — describing what the software
is a port of is nominative use, and there is no accurate alternative. The
shipped artifacts do not: the project, the application, the macOS bundle and
the bundle identifier are all "pikpik-2". Trademark protects against confusion
about origin, so nothing built from this repository should present itself as a
Nintendo product. Please keep it that way if you fork.

The icon is an original drawing generated by `assets/make_icon.py`, so no
artwork of contested provenance appears anywhere in the build.

## 7. If you are a rights holder

Open an issue at https://github.com/magikarp/pikpik-2, or use GitHub's
designated agent process. A notice identifying specific material will be acted
on promptly and without argument; the maintainers would rather take something
down than litigate it.

Note what is actually here: an original port layer and a set of scripts. The
decompiled sources are hosted by
[projectPiki/pikmin2](https://github.com/projectPiki/pikmin2), a separate
project with separate maintainers, outside this project's control.

## 8. If you contribute

- Do not submit game assets, disc data or extracted binaries. Ever.
- Do not paste large excerpts of decompiled source into issues or pull
  requests. Reference file and line in the upstream project instead.
- Changes to the game tree belong in `tools/prepare_source.py` and its
  companions, never as copied-in files.
- Contribute only work you wrote yourself.

See [CONTRIBUTING.md](../CONTRIBUTING.md).

## 9. Warranty

None. See LICENSE. This is a hobby project driving a decompiled
1990s-toolchain codebase through a modern optimising compiler, and it is
mid-development. Treat every output as provisional.
