#!/bin/bash
# setup.sh — fetch the two upstream projects this port builds on.
#
#   ./setup.sh              # fetch into vendor/, at the pinned commits
#   ./setup.sh --check      # verify an existing vendor/, change nothing
#   ./setup.sh --force      # discard vendor/ and refetch
#
# WHY THIS EXISTS
#
# This repository contains the port layer and nothing else. The decompiled
# game sources and Aurora belong to separate upstream projects and are fetched
# from them here, at pinned commits, rather than copied in. docs/LEGAL.md
# section 4 explains the reasoning.
#
# What you end up with:
#
#   vendor/pikmin2/     the decompilation, at upstream-revision.txt
#   vendor/aurora/      Aurora, at aurora-revision.txt
#
# Neither is modified by the build. tools/prepare_source.py and
# tools/prepare_renderer.py copy the pinned revisions into ignored build
# directories and apply this port's changes there.
#
# vendor/ is gitignored. Nothing this script produces belongs in a commit.
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"

DECOMP_REPO="${DECOMP_REPO:-https://github.com/projectPiki/pikmin2.git}"
AURORA_REPO="${AURORA_REPO:-https://github.com/encounter/aurora.git}"
DECOMP_COMMIT="$(tr -d '[:space:]' < upstream-revision.txt)"
AURORA_COMMIT="$(tr -d '[:space:]' < aurora-revision.txt)"

MODE=apply
while [ $# -gt 0 ]; do
    case "$1" in
        --check) MODE=check ;;
        --force) MODE=force ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v git >/dev/null || { echo "git not found" >&2; exit 1; }
say()  { printf '==> %s\n' "$*"; }
fail() { printf 'setup.sh: %s\n' "$*" >&2; exit 1; }

# Fetch one commit rather than cloning history: a fraction of the transfer,
# and pinning by SHA means there is nothing else we would ever want.
fetch_pinned() {  # dir, repo, commit, label
    local dir="$1" repo="$2" commit="$3" label="$4"
    say "$label at ${commit:0:12}"
    if [ ! -d "$dir/.git" ]; then
        rm -rf "$dir"
        mkdir -p "$dir"
        git init -q "$dir"
        git -C "$dir" remote add origin "$repo"
    fi
    if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
        git -C "$dir" fetch -q --depth 1 origin "$commit" \
            || fail "could not fetch $commit from $repo
    Upstream may have rewritten history, or you may be offline.
    Nothing in this repository can substitute for it."
    fi
    git -C "$dir" -c advice.detachedHead=false checkout -q --force "$commit"
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] \
        || fail "$dir is not at $commit after checkout"
}

verify() {
    local missing=0
    [ -f "$ROOT/vendor/pikmin2/src/plugProjectYamashitaU/AIConstant.cpp" ] \
        || [ -d "$ROOT/vendor/pikmin2/src" ] || { echo "    MISSING vendor/pikmin2/src" >&2; missing=1; }
    [ -f "$ROOT/vendor/aurora/CMakeLists.txt" ] \
        || { echo "    MISSING vendor/aurora/CMakeLists.txt" >&2; missing=1; }
    [ "$missing" = 0 ] || fail "vendor/ is incomplete — try ./setup.sh --force"
    say "verified: vendor/ looks complete"
}

case "$MODE" in
    check)
        for pair in "vendor/pikmin2:$DECOMP_COMMIT" "vendor/aurora:$AURORA_COMMIT"; do
            d="${pair%%:*}"; c="${pair##*:}"
            [ -d "$d/.git" ] || fail "$d does not exist — run ./setup.sh"
            got="$(git -C "$d" rev-parse HEAD 2>/dev/null || echo none)"
            [ "$got" = "$c" ] || fail "$d is at $got, expected $c — run ./setup.sh --force"
        done
        verify; exit 0 ;;
    force) rm -rf vendor ;;
esac

fetch_pinned vendor/pikmin2 "$DECOMP_REPO" "$DECOMP_COMMIT" "decompilation"
fetch_pinned vendor/aurora  "$AURORA_REPO" "$AURORA_COMMIT" "Aurora"
verify

cat <<MSG

Setup complete.

  decompilation   vendor/pikmin2  (${DECOMP_COMMIT:0:12})
  renderer        vendor/aurora   (${AURORA_COMMIT:0:12})
  next            ./build_mac.sh

You still need game data. See docs/GAME_DATA.md — it is not included and
cannot be, and the build does nothing useful without it.
MSG
