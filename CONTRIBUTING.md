# Contributing

Contributions are welcome. A few rules are not negotiable, and they come first
because breaking one is the only kind of pull request that cannot be fixed in
review.

## The hard rules

**No game assets. Ever.** No textures, models, audio, video, save files from a
retail disc, disc images, or extracted data — in a commit, in an issue, in a
screenshot of a file listing, in a test fixture, anywhere. A pull request
containing any of it gets closed rather than amended, because the history would
need rewriting.

**No decompiled sources in this repository, and no diffs of them.** Changes to
the game tree go in `tools/prepare_source.py` and its companions, as
transformations. That is what keeps this repository free of upstream excerpts
entirely — see [docs/LEGAL.md](docs/LEGAL.md) section 4. `vendor/` is
gitignored; keep it that way.

**Do not paste large excerpts of decompiled code into issues or pull request
descriptions.** A file and line reference into the upstream project does the
same job. A few lines to make a point is fine; a function body is not.

**Contribute only work you wrote.** Do not copy code in from other ports,
emulators or decompilation projects, however permissively licensed, without
saying so explicitly, so its licence and attribution can be handled properly.

## Where to send things

| Change | Goes to |
|---|---|
| The decompilation — better names, better matches | [projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) |
| The GX/Metal layer | [Aurora](https://github.com/encounter/aurora) |
| The port — disc, audio, saves, host J3D/J2D, build, docs | here |

This project tracks pinned commits of the first two and benefits from anything
that lands there.

## Before you open a pull request

**The game is mid-debugging.** Roughly the first half has been played through
and fixed up; the later game has not. A bug report from the back half is useful
on its own, and does not need a fix attached.

**Replay, do not merely compile.** A recording plays through and the result
matches, or the change is not verified. `tools/input_replay.sh`. "It builds and
seems fine" is not a claim this codebase supports — too much of its behaviour
is emergent from hardware emulation that no compiler checks.

**Keep the warning gates clean.** `tools/warning_census.py` against
`tools/warning_baseline.txt`. If your change adds a warning in a gated class,
that is the gate working.

**Say what you measured.** A claim comes with the observation supporting it.
"Fixed the flicker" is much less useful than the number that was wrong and the
number it is now. The second can be checked a year later.

## Style

Match the file you are in. The port layer favours long explanatory comments
over short clever code, particularly where something is non-obvious because the
hardware was strange — that comment is often the only place the knowledge
exists. Keep them.

## Reporting a bug

Include: macOS version, build type, where you are in the game, and what you
were doing. A recording (`tools/input_record.sh`) is worth more than any
description — it is the difference between a report someone can reproduce and
one they can only sympathise with.

## Licence

Contributions are accepted under the repository's MIT licence. See
[LICENSE](LICENSE) and [docs/LEGAL.md](docs/LEGAL.md).
