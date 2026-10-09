#!/usr/bin/env python3
"""Check every engine translation unit and retain diagnostics, without a dump."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--source-root", type=Path, required=True)
parser.add_argument("--jobs", type=int, default=8)
parser.add_argument("--filter", default="", help="Only check source paths containing this text")
parser.add_argument("--error-limit", type=int, default=5)
args = parser.parse_args()
entries = json.loads((args.build / "compile_commands.json").read_text())
entries = [e for e in entries if "p2_engine.dir" in e.get("command", " ".join(e.get("arguments", []))) ]
if args.filter:
    entries = [e for e in entries if args.filter in e["file"]]
if not entries:
    parser.error("No p2_engine commands found; configure this build directory first")
if args.jobs < 1:
    parser.error("--jobs must be positive")

def check(entry):
    command = entry.get("arguments") or shlex.split(entry["command"])
    output = command.index("-o")
    del command[output:output + 2]
    command.remove("-c")
    command += ["-fsyntax-only", f"-ferror-limit={args.error_limit}", "-fno-color-diagnostics"]
    result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    return {"file": str(Path(entry["file"]).relative_to(args.source_root.resolve())),
            "passed": result.returncode == 0, "diagnostics": result.stderr}

results = []
with ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for result in pool.map(check, entries):
        results.append(result)
        if len(results) % 100 == 0:
            print(f"Checked {len(results)}/{len(entries)}", flush=True)
passed = sum(r["passed"] for r in results)
errors = Counter(line.split(" error:", 1)[1].strip()
                 for result in results for line in result["diagnostics"].splitlines()
                 if " error:" in line and "too many errors emitted" not in line)
report = {"prepared_source": str(args.source_root.resolve()),
          "source_signature": (args.source_root / ".p2-prepared").read_text(),
          "passed": passed, "total": len(results),
          "common_errors": errors.most_common(20), "results": results}
path = args.build / ("compile-census-filtered.json" if args.filter else "compile-census.json")
path.write_text(json.dumps(report, indent=2) + "\n")
print(f"Engine syntax census: {passed}/{len(results)} pass. Report: {path}")
for error, count in errors.most_common(8):
    print(f"  {count}: {error}")
# A census is an inventory, not a passing engine build. Failed entries are
# explicitly recorded; use the p2_engine target as the actual compile gate.
