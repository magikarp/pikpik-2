#!/bin/zsh
# Benchmark the renderer by replaying one recording uncapped across settings.
#
#   tools/bench.sh [recording] [build-dir]          (default: bench-day1, build-opt)
#   SCALES="1 2 4" MSAA="1 4" PACK=0 tools/bench.sh
#
# Each run replays recordings/<name>.rec from its save snapshot with P2_BENCH=1:
# offscreen (nothing presented, so the display refresh does not pace frames) and
# no 60 Hz retrace pacing. Replays index by game tick, so every row renders the
# same frames. Audio renders silently (SDL dummy driver) and runs in real time.
set -u
ROOT="${0:A:h:h}"
name="${1:-bench-day1}"
build="${2:-build-opt}"
rec="$ROOT/recordings/$name.rec"; snap="$ROOT/recordings/$name.save"
bin="$ROOT/$build/p2_story_bringup.app/Contents/MacOS/p2_story_bringup"
pack="${PACK_DIR:-$ROOT/../references/GPV}"
[[ -f "$rec" ]] || { print -u2 "no recording at $rec"; exit 1; }
[[ -x "$bin" ]] || { print -u2 "no binary at $bin"; exit 1; }
last=$(awk '!/^#|^save|^seed/ {t=$1} END {print t}' "$rec")
packs=(0); [[ "${PACK:-1}" == 1 && -d "$pack" ]] && packs=(0 1)
work="${TMPDIR:-/tmp}/pikmin2-bench-$$"; mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
print "recording $name (ticks 0-$last), build $build"
printf "%-6s %-5s %-5s %10s %8s %8s %8s %8s  %s\n" scale msaa pack fps avg_ms p95_ms p99_ms max_ms render
for scale in ${=SCALES:-1 2 3 4}; do for msaa in ${=MSAA:-1 4}; do for usePack in $packs; do
    rm -rf "$work/card"; mkdir -p "$work/card"; [[ -d "$snap" ]] && cp -R "$snap/." "$work/card/"
    extra=(); [[ $usePack == 1 ]] && extra=(P2_TEXTURE_PACK="$pack")
    line=$(env SDL_AUDIO_DRIVER=dummy P2_BENCH=1 P2_RENDER_SCALE=$scale P2_MSAA=$msaa P2_INPUT_PLAY="$rec" \
        P2_CARD_DIRECTORY="$work/card" P2_EXIT_TICK=$last $extra "$bin" 2>&1 >/dev/null | grep "^\[BENCH\]")
    if [[ -z "$line" ]]; then printf "%-6s %-5s %-5s %s\n" $scale $msaa $usePack "*** run failed (no summary)"; continue; fi
    fps=$(sed -E 's/.* ([0-9.]+) fps.*/\1/' <<<"$line")
    read avg p95 p99 mx <<<$(sed -E 's/.*avg ([0-9.]+) p50 [0-9.]+ p95 ([0-9.]+) p99 ([0-9.]+) max ([0-9.]+).*/\1 \2 \3 \4/' <<<"$line")
    render=$(sed -E 's/.*render ([0-9]+x[0-9]+).*/\1/' <<<"$line")
    printf "%-6s %-5s %-5s %10s %8s %8s %8s %8s  %s\n" $scale $msaa $usePack $fps $avg $p95 $p99 $mx $render
done; done; done
