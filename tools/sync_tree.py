"""Mirror a freshly generated tree into a stable build path.

Only files whose bytes differ are rewritten, so unchanged sources keep their
timestamps and Ninja recompiles just the translation units a patch touched.
"""
import os
from pathlib import Path
import shutil


def sync_tree(staging: Path, output: Path) -> int:
    """Make `output` identical to `staging`; return the number of files written or removed."""
    changed = 0
    wanted = set()
    for file in staging.rglob("*"):
        relative = file.relative_to(staging)
        target = output / relative
        wanted.add(relative)
        if file.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        data = file.read_bytes()
        if target.is_file() and target.read_bytes() == data:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        shutil.copymode(file, target)
        changed += 1
    for file in sorted(output.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if file.relative_to(output) in wanted:
            continue
        if file.is_dir():
            os.rmdir(file) if not any(file.iterdir()) else None
        else:
            file.unlink()
            changed += 1
    return changed
