#!/bin/zsh
# Build Pikmin 2 for iPad (arm64, iOS 16.4: the first with BC7 on M-series GPUs).
#
# Produces build-ios/: the game's static archives, plus Pikmin2.xcconfig listing
# them with their frameworks. ios/Pikmin2.xcodeproj includes that file, compiles
# main.m, and owns signing and deployment. p2_ios_link_check performs the same
# link here, so a missing symbol fails in this script rather than in Xcode.
#
# Dawn (source, built for 16.4 so BC textures are compiled in) and SDL3 come from
# ../references (see cmake/renderer.cmake). Dawn's protobuf is fuzzer/IR-binary only
# and needs a host protoc when cross-compiling, so it is off.
# Unix Makefiles, not Ninja: ios_link_config.py reads the Makefile link line.
# Static everything (an iOS app cannot load loose dylibs). Homebrew is ignored: its macOS libraries (zstd via pkg-config) cannot link for iOS.
set -eu
ROOT="${0:A:h}"
BUILD="$ROOT/build-ios"
# Xcode shell name — the published tree ships under a different one.
IOS_SHELL="${IOS_SHELL:-Pikmin2}"
# Optional per-tree overrides. Absent here; the published tree ships one so it
# builds under its own name without editing this script.
[ -f "$ROOT/build.conf" ] && . "$ROOT/build.conf"
cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DP2_BUILD_RENDERER=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
    -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_IR_BINARY=OFF \
    -DP2_IOS_SHELL_NAME="$IOS_SHELL" "$@"
cmake --build "$BUILD" --target p2_ios_link_check -j"$(sysctl -n hw.ncpu)"
print "\niOS build complete: open ios/$IOS_SHELL.xcodeproj and run on the device."
