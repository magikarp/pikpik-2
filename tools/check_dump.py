#!/usr/bin/env python3
"""Read-only identification of the US GameCube dump used by this port."""
import argparse
import hashlib
from pathlib import Path
import struct

# config/GPVE01/config.yml at the pinned decomp revision.
EXPECTED_DOL_SHA1 = "90d328bf8f190c90472e8c19e7e53c6ad0fe0d1a"


def inspect(path):
    path = Path(path)
    extracted = path.is_dir()
    header_path = path / "sys/boot.bin" if extracted else path
    if not header_path.is_file():
        raise ValueError(f"Missing {header_path}; extracted dumps need sys/boot.bin and sys/main.dol")
    with header_path.open("rb") as stream:
        header = stream.read(0x440)
    if len(header) < 0x440 or header[0x1c:0x20] != bytes.fromhex("c2339f3d"):
        raise ValueError("Not a raw GameCube disc header. Convert compressed RVZ/WIA images with Dolphin first.")
    game_id = header[:6].decode("ascii", errors="replace")
    if game_id != "GPVE01":
        raise ValueError(f"This build targets US GPVE01; found {game_id}")
    print(f"Game ID: {game_id}; disc {header[6]}, revision {header[7]}")
    if extracted:
        dol = path / "sys/main.dol"
        if not dol.is_file():
            raise ValueError(f"Missing {dol}")
        with dol.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha1").hexdigest()
        if digest != EXPECTED_DOL_SHA1:
            raise ValueError(f"DOL does not match the pinned US decomp: {digest}")
        print("Executable SHA-1 matches the pinned US decomp.")
        if not (path / "files").is_dir():
            raise ValueError("Executable verified, but files/ is missing. Extract the complete disc, not just system data.")
        print(f"Extracted asset root: {path / 'files'}")
    else:
        dol_offset, fst_offset, fst_size = struct.unpack_from(">III", header, 0x420)
        size = path.stat().st_size
        if dol_offset < 0x2440 or dol_offset + 0x100 > size or fst_offset < 0x2440 or fst_size < 12 or fst_offset + fst_size > size:
            raise ValueError("Disc executable/filesystem offsets are invalid or the image is truncated")
        print("Raw disc header and filesystem bounds are valid; executable hash and assets still need extraction/verification.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="ISO/GCM or extracted disc root")
    args = parser.parse_args()
    try:
        inspect(args.path)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Dump check failed: {error}\n")
