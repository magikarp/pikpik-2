#!/bin/zsh
# Replay a recorded session against its own save snapshot.
#
#   tools/input_replay.sh <name> [build-dir] [VAR=value ...]
#
# The snapshot is copied to a scratch card directory so the recording's save
# stays untouched. Examples:
#   tools/input_replay.sh day1
#   tools/input_replay.sh day1 build-metal P2_TRACE_TICKS=500 P2_EXIT_TICK=9000
set -u
ROOT="${0:A:h:h}"
name="${1:-}"
build="${2:-build-opt}"
shift 2 2>/dev/null || shift $#
[[ -n "$name" ]] || { print -u2 "usage: tools/input_replay.sh <name> [build-dir] [VAR=value ...]"; exit 2; }
rec="$ROOT/recordings/$name.rec"; snap="$ROOT/recordings/$name.save"
bin="$ROOT/$build/p2_story_bringup.app/Contents/MacOS/p2_story_bringup"
[[ -f "$rec" ]] || { print -u2 "no recording at $rec"; exit 1; }
[[ -x "$bin" ]] || { print -u2 "no binary at $bin"; exit 1; }
# A private card copy per run, so concurrent replays never share a save.
card=$(mktemp -d "${TMPDIR:-/tmp}/pikmin2-replay-$name.XXXXXX") || exit 1
trap 'rm -rf "$card"' EXIT
[[ -d "$snap" ]] && cp -R "$snap/." "$card/"
env P2_INPUT_PLAY="$rec" P2_CARD_DIRECTORY="$card" "$@" "$bin"
