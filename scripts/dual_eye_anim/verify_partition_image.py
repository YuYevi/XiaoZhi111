#!/usr/bin/env python3
"""End-to-end check of the eye-animation assets partition image.

Runs the same generator that spiffs_create_partition_assets() invokes at build
time, then parses the produced image with the exact layout rules
main/boards/AI-dual-eyed-doll/board_animations.cc uses. It catches a wrong table
stride (CONFIG_MMAP_FILE_NAME_LENGTH), a bad payload offset, or a clip set that no
longer fits the partition - all before flashing.

    python3 scripts/dual_eye_anim/verify_partition_image.py

Uses the ESP-IDF Python environment, because the generator needs Pillow. Run it
from a shell that has already sourced export.sh if the modules are missing.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

BOARD_DIR = os.path.join(ROOT, "main", "boards", "AI-dual-eyed-doll")
SRC = os.path.join(BOARD_DIR, "partition_assets")
GEN = os.path.join(
    ROOT, "managed_components", "espressif__esp_mmap_assets", "py_tool", "spiffs_assets_gen.py"
)
OUT_DIR = os.path.join(ROOT, "tmp", "verify_partition_image")
IMG = os.path.join(OUT_DIR, "assets", "assets.bin")

# Must match partitions/v2/4m_model.csv and sdkconfig.
PARTITION_SIZE = 0x100000
NAME_LENGTH = 16  # CONFIG_MMAP_FILE_NAME_LENGTH
ENTRY_SIZE = NAME_LENGTH + 4 + 4 + 2 + 2
MAGIC = b"DEA1"

EXPECTED = [
    "cute.anim.z",
    "cuteblnk.anim.z",
    "happy.anim.z",
    "hapyblnk.anim.z",
    "sad.anim.z",
    "shckblnk.anim.z",
]


def build_image() -> None:
    """Drive the generator exactly the way project_include.cmake does."""
    import json

    os.makedirs(os.path.dirname(IMG), exist_ok=True)
    config = {
        "project_dir": ROOT,
        "include_path": OUT_DIR,
        "assets_path": SRC,
        "image_file": IMG,
        "lvgl_ver": "9.5.0",
        "assets_size": hex(PARTITION_SIZE),
        "support_format": ".anim.z",
        "name_length": str(NAME_LENGTH),
        "split_height": "0",
        "support_spng": False,
        "support_sjpg": False,
        "support_qoi": False,
        "support_sqoi": False,
        "support_pjpg": False,
        "support_raw": False,
        "support_raw_dither": "false",
        "support_raw_bgr": "false",
        "support_raw_ff": "BIN",
        "support_raw_cf": "ARGB8888",
        "app_bin_path": "",
        "pjpg_processor": "",
        "header_asset_name": "assets",
        "source_bin_path": "",
    }
    config_path = os.path.join(OUT_DIR, "assets_config.json")
    with open(config_path, "w", encoding="utf-8") as fh:
        json.dump(config, fh, indent=2)

    result = subprocess.run(
        [sys.executable, GEN, "--config", config_path, "--build"],
        capture_output=True,
        text=True,
    )
    print(result.stdout.strip())
    if result.returncode != 0:
        print(result.stderr.strip(), file=sys.stderr)
        sys.exit("generator failed")


def parse_like_firmware() -> dict:
    """Mirror BoardAnimations::Get() byte for byte."""
    with open(IMG, "rb") as fh:
        blob = fh.read()

    count = struct.unpack_from("<I", blob, 0)[0]
    checksum = struct.unpack_from("<I", blob, 4)[0]
    combined_len = struct.unpack_from("<I", blob, 8)[0]
    print(f"\nimage      {len(blob)} B ({len(blob) / 1024:.1f} KB), partition {PARTITION_SIZE} B")
    print(f"header     count={count} checksum=0x{checksum:04X} combined_len={combined_len}")
    print(f"stride     name={NAME_LENGTH} + 12 = {ENTRY_SIZE} B, table is {count * ENTRY_SIZE} B")

    assert len(blob) <= PARTITION_SIZE, f"image is {len(blob)} B, partition only {PARTITION_SIZE} B"

    # The checksum covers everything from offset 12 to the end of the image.
    calc = sum(blob[12:]) & 0xFFFF
    print(f"checksum   stored 0x{checksum:04X} vs computed 0x{calc:04X} "
          f"{'OK' if calc == checksum else 'MISMATCH'}")
    assert calc == checksum, "checksum mismatch, the image is corrupt"

    assert 12 + count * ENTRY_SIZE <= len(blob), "table runs past the end of the image"
    assert 12 + combined_len <= len(blob) + 1, "combined_len exceeds the image length"

    table = 12
    payloads = table + count * ENTRY_SIZE
    found = {}
    for i in range(count):
        entry = table + i * ENTRY_SIZE
        name = blob[entry : entry + NAME_LENGTH].split(b"\0")[0].decode()
        size, offset = struct.unpack_from("<II", blob, entry + NAME_LENGTH)
        width, height = struct.unpack_from("<HH", blob, entry + NAME_LENGTH + 8)
        payload = payloads + offset
        assert blob[payload : payload + 2] == b"ZZ", f"{name}: missing payload magic at {payload}"
        assert payload + 2 + size <= len(blob), f"{name}: payload runs past the end of the image"
        magic = blob[payload + 2 : payload + 6]
        _version, frames, w, h, delay_ms, _reserved = struct.unpack_from(
            "<HHHHHH", blob, payload + 6
        )
        found[name] = (size, width, height)
        ok = "OK " if magic == MAGIC else "BAD"
        print(f"  [{ok}] {name:<14} entry={width}x{height} blob={size / 1024:7.1f} KB  "
              f"magic={magic.decode(errors='replace')} frames={frames} {delay_ms}ms {w}x{h}")

    assert sorted(found) == sorted(EXPECTED), f"unexpected file set: {sorted(found)}"
    assert all(w == 0 and h == 0 for _, w, h in found.values()), "blobs should carry no dimensions"
    print(f"\nAll {len(found)} animations parsed back correctly, layout matches board_animations.cc")
    return found


def main() -> int:
    build_image()
    parse_like_firmware()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
