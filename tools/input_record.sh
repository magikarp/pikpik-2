#!/bin/zsh
# Record a play session for headless replay.
#
#   tools/input_record.sh <name> [build-dir]
#
# Play normally; quit the window when done. Writes recordings/<name>.rec
# (flushed on every input change, so a crash keeps the tail) and snapshots the
# card directory as recordings/<name>.save, because the save is part of the
# initial conditions. The recording also stores the boot RNG seed.
set -u
ROOT="${0:A:h:h}"
name="${1:-}"
build="${2:-build-opt}"
[[ -n "$name" ]] || { print -u2 "usage: tools/input_record.sh <name> [build-dir]"; exit 2; }
bin="$ROOT/$build/p2_story_bringup.app/Contents/MacOS/p2_story_bringup"
[[ -x "$bin" ]] || { print -u2 "no binary at $bin (cmake --build $build --target p2_story_bringup)"; exit 1; }

out="$ROOT/recordings"; mkdir -p "$out"
rec="$out/$name.rec"; snap="$out/$name.save"
# Never overwrite a recording: it cost a play session.
if [[ -e "$rec" ]]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    mv "$rec" "$out/$name-$stamp.rec"; [[ -d "$snap" ]] && mv "$snap" "$out/$name-$stamp.save"
    print "existing recording archived as $name-$stamp.rec"
fi

card="${P2_CARD_DIRECTORY:-$ROOT/userdata/cards}"
mkdir -p "$snap"; [[ -d "$card" ]] && cp -R "$card/." "$snap/"
print "recording -> $rec\nsave snap -> $snap (from $card)\nPlay; close the window when done."
P2_INPUT_REC="$rec" P2_CARD_DIRECTORY="$card" "$bin"
print "recorded $(grep -vc '^#\|^save \|^seed ' "$rec") input changes; replay: tools/input_replay.sh $name"
