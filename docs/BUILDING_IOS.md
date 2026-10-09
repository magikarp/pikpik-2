# Building and running on iPadOS / iOS

Written as a step-by-step, assuming you have Xcode and an Apple Developer
account but have not done this particular dance before. Reasoning is in the
notes at the end.

**Target:** an M-series iPad, or an iPhone, running **iPadOS/iOS 16.4 or
later**. Developed and tested on an iPad Pro 11" (M1). The 16.4 floor is not
arbitrary — see the Dawn note below.

**Xcode 16 or newer is required**, and this is a hard floor rather than a
preference: the project file is `objectVersion = 77` and uses
`PBXFileSystemSynchronizedRootGroup`, the folder-backed group format Xcode 16
introduced, so older Xcode cannot open it at all.

**Simulator: no.** The build targets `iphoneos` on arm64. Run it on hardware.

---

## Step 1 — Get the sources

```bash
git clone https://github.com/magikarp/pikpik-2.git && cd pikpik-2
./setup.sh
```

## Step 2 — Build the engine

```bash
./build_ios.sh
```

**The first run takes 20 to 40 minutes.** It compiles Dawn from source, which
is the bulk of that. Later runs take a few minutes.

What it produces: the game's static archives in `build-ios/`, plus a generated
`build-ios/pikpik-2.xcconfig` listing every archive and framework in the order
CMake linked them. It also performs the same link itself, through the
`p2_ios_link_check` target, so a missing symbol fails here with a readable
error rather than inside Xcode.

Everything is static. An iOS app cannot load loose dylibs, and Homebrew is
deliberately ignored — its macOS libraries cannot link for iOS.

## Step 3 — Open the Xcode project

```bash
open ios/pikpik-2.xcodeproj
```

The project is deliberately thin. Xcode compiles exactly one file,
`ios/pikpik-2/main.m`; everything else arrives through the generated xcconfig.
Xcode owns signing, the `Info.plist`, the asset catalog and deployment, and
nothing else.

Xcode creates a scheme the first time you open the project. That is expected —
schemes are per-developer state and are not committed.

## Step 4 — Signing

This part is yours. Select the target, then **Signing & Capabilities**:

| Field | What to do |
|---|---|
| **Automatically manage signing** | Leave checked |
| **Team** | Pick yours. It ships blank on purpose — a hardcoded team id fails for everyone not in it |
| **Bundle Identifier** | **Change it.** It ships as `com.example.pikpik2`, which will not register. Use your own reverse-DNS |

**Then do not change the bundle identifier again.** iOS treats a new bundle id
as a new app with an empty Documents folder, and you will be re-copying the
disc. Decide once.

A free account signs apps that expire after seven days; a paid one lasts a
year. On a modern iOS the device also needs **Settings → Privacy & Security →
Developer Mode** turned on, and the first install from a new certificate needs
it trusted under **Settings → General → VPN & Device Management**.

## Step 5 — Copy the game data onto the device

Launch once first, so the app creates its folders.

Then in **Finder → your iPad → Files → pikpik-2**, drag these into the app's
Documents folder:

| Folder | Contents | Required |
|---|---|---|
| `disc/` | the extracted `GPVE01` disc — `sys/` and `files/` directly inside | yes |
| `textures/` | a Dolphin-format texture pack | no |
| `recordings/` | input recordings, for the bench | no |

The disc is roughly 1.3 GB over a slow path. Expect it to take a while.

The app creates `cards/` — the memory card — on first launch. Game data is not
provided and cannot be; see [GAME_DATA.md](GAME_DATA.md) for what it is and
which revision is required.

## Step 6 — Settings

`Documents/p2_settings.txt` overrides any `P2_*` setting, one `KEY=VALUE` per
line:

```
P2_MSAA=2
P2_TEXTURE_PACK=0
```

Defaults are the screen's native resolution, MSAA 4, and the texture pack when
one is present.

---

## Notes worth reading before you lose an afternoon

**Why Dawn is built from source.** Aurora can use a prebuilt Dawn package, and
the macOS build does. iOS cannot: the published package targets iOS 14.0, and
Dawn only compiles BC texture support when the deployment target is 16.4 or
later. The texture packs are BC7, so a prebuilt Dawn would abort the first time
it created a BC texture.

There is a second wrinkle. Dawn enables the `TextureCompressionBC` feature on
iOS 16.4+, but its BC pixel-format tables are still guarded macOS-only, so the
feature reports as available and then aborts in `MetalPixelFormat`.
`tools/patch_dawn.py` widens those two guards to match the condition the
feature itself uses. The build applies it automatically when Dawn is fetched.
It is idempotent, and it fails loudly if upstream Dawn drifts away from what it
expects rather than silently patching nothing.

**Xcode does not know about the CMake build.** After changing anything in
`src/` or in the prepared game tree, re-run `./build_ios.sh` before pressing
Run. Otherwise Xcode links yesterday's archives and you debug a bug you already
fixed. This catches everyone at least once.

**Unix Makefiles, not Ninja.** `tools/ios_link_config.py` reads the link line
out of the generated Makefile to build the xcconfig. Switching the generator
breaks that.

## Troubleshooting

**"Signing for ... requires a development team"** — Step 4.

**"Failed to register bundle identifier"** — you left it as
`com.example.pikpik2`.

**Device missing from the destination menu** — Developer Mode is off, or the
device is below iOS 16.4.

**`ld: library not found`** in Xcode — `./build_ios.sh` has not been run, or it
failed. Xcode links what the xcconfig names, and only CMake produces those
archives.

**`patch_dawn: drift in ...`** — upstream Dawn has moved and the BC patch no
longer matches. Open an issue; do not work around it by skipping the patch, or
BC textures will abort at runtime.

**The app launches and immediately quits** — almost always the disc. Check
`Documents/disc/sys/` and `Documents/disc/files/` exist on the device.

**An empty Documents folder after a reinstall** — you changed the bundle
identifier. See Step 4.
