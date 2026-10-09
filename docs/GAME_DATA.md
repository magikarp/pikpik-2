# Game data

The build produces an engine with nothing to run. The game's content — levels,
models, textures, audio, video — lives on the retail disc, and it is not here
and never will be. See [LEGAL.md](LEGAL.md) section 5.

You supply it, from a disc you own.

## What the port expects

The **extracted disc filesystem**: a directory containing `sys/` and `files/`.
Not an ISO, not an RVZ or WIA, not a compressed image — the unpacked tree.

```
disc/
  sys/
    boot.bin
    fst.bin
    main.dol
  files/
    ...
```

It must be *Pikmin 2*, USA — disc identifier `GPVE01`. That is the revision the
decompilation matches, and the only one this port is built against. The port
reads `sys/boot.bin` on startup purely to check that identifier, and refuses
anything else rather than failing in some interesting way later.

Anything you legally obtain the data with is between you and your
jurisdiction. This project does not tell you how, and ships nothing that helps.

## Where it goes

Either of these:

1. **`disc/` in the repository root.** This is the default `build_mac.sh`
   compiles in, and the simplest arrangement.
2. **`P2_DISC_DIRECTORY`** — an environment variable holding the path.
   Overrides the compiled default, and avoids a rebuild if you move the data.

```bash
P2_DISC_DIRECTORY=/Volumes/Games/pikmin2-extracted open build/pikpik-2.app
```

A symlink at `disc/` works too, if you would rather not move 1.3 GB.

## Checking it

```bash
python3 tools/check_dump.py disc
```

This verifies the structure and checks `sys/main.dol` against the SHA-1 of the
revision the decompilation targets. Identifying the ISO alone does not verify
the executable, which is why the check exists.

`check_dump.py` is the only thing in this project that reads `main.dol`, and it
is a tool you run deliberately. The game itself never opens it — see
[LEGAL.md](LEGAL.md) section 5.
