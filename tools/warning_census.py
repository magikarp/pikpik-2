#!/usr/bin/env python3
"""Census of 64-to-32-bit truncations in the prepared game source.

The 218 known sites are sizes, pointer differences, string lengths and timer
values (audited 2026-10-02: no absolute address is truncated). Too many to
cast one by one, so they are recorded here and only new sites fail.

    tools/warning_census.py [build-dir]            compare against the baseline
    tools/warning_census.py [build-dir] --update   rewrite the baseline

Sites are keyed by file and source text, not line number, so unrelated edits
do not churn the baseline.
"""
import collections
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

FLAGS = ["-fsyntax-only", "-Wno-everything", "-Wshorten-64-to-32"]
ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "warning_baseline.txt"


def compile_jobs(build):
    seen = set()
    for entry in json.loads((build / "compile_commands.json").read_text()):
        path = entry["file"]
        if "/prepared/current/src/" not in path or path in seen:
            continue
        seen.add(path)
        args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        command, skip = [], False
        for arg in args:
            if skip:
                skip = False
            elif arg in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif arg not in ("-c", "-MD", "-MMD") and not arg.startswith("-Werror"):
                command.append(arg)
        yield command + FLAGS, entry["directory"]


def census(build):
    sites = collections.Counter()
    found = set()
    def run(job):
        return subprocess.run(job[0], cwd=job[1], capture_output=True, text=True).stderr
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for stderr in pool.map(run, compile_jobs(build)):
            for match in re.finditer(r"^(\S+?):(\d+):\d+: warning: .*\[-Wshorten-64-to-32\]", stderr, re.M):
                if (match[1], match[2]) in found:
                    continue  # the same header line, reported by several translation units
                found.add((match[1], match[2]))
                source = Path(match[1]).read_text(errors="replace").split("\n")[int(match[2]) - 1]
                path = match[1].split("/prepared/current/", 1)[-1]
                sites[f"{path}\t{' '.join(source.split())}"] += 1
    return sites


def main():
    build = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else ROOT / "build-opt"
    sites = census(build.resolve())
    if "--update" in sys.argv:
        BASELINE.write_text("".join(f"{count}\t{key}\n" for key, count in sorted(sites.items())))
        print(f"baseline: {sum(sites.values())} truncations")
        return 0
    known = collections.Counter()
    for line in BASELINE.read_text().splitlines():
        count, key = line.split("\t", 1)
        known[key] = int(count)
    new = sites - known
    print(f"{sum(sites.values())} truncations, {sum(known.values())} in baseline, {sum(new.values())} new")
    for key in sorted(new):
        path, text = key.split("\t", 1)
        print(f"*** new 64-to-32 truncation: {path}: {text}")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
