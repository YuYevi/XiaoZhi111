#!/usr/bin/env python3
"""Pack 160x160 GIF animations into the DualEyeDisplay frame-delta asset format.

The AI-dual-eyed-doll board has 4MB flash, so a full RGB565 frame (51,200 bytes)
per animation frame is far too expensive to store raw. Instead each frame is
stored as a byte-wise delta against the previous frame:

    delta[i] = (frame[i] - previous[i]) & 0xFF

Cartoon eye animations are mostly static between frames, so the delta is almost
all zeros and zlib collapses it. Measured on the shipped assets:

    shy_crop_160_black.gif   269 frames   1,231 KB raw-zlib ->  61 KB delta-zlib
    cute_crop_160_black.gif  132 frames     681 KB raw-zlib -> 504 KB delta-zlib

The source GIFs draw their artwork inside a smaller centred square on a black
canvas (124x124 inside 160x160 for the shipped pair), which shows up as a black
ring around the eyes on the panel. By default the packer takes the union of the
non-black bounds across every frame, crops to it, and scales the result back up
to the panel size so the artwork fills the screen. Pass --no-trim to keep the
original canvas.

Output file layout (all integers little-endian):

    offset  size            field
    0       4               magic 'D','E','A','1'
    4       2               format version (1)
    6       2               frame count
    8       2               width
    10      2               height
    12      2               frame delay in milliseconds
    14      2               reserved, zero
    16      4 * frame_count per-frame compressed size in bytes
    ...                     zlib streams, one per frame, each inflating to
                            width * height * 2 delta bytes

The first frame is a delta against an all-zero buffer, so it is losslessly the
absolute first frame. The decoder only ever adds, so a delta byte of 0xFF
wrapping past zero is handled by the same byte-wise addition.

Usage:
    python3 scripts/dual_eye_anim/pack_eye_anim.py \
        main/boards/AI-dual-eyed-doll/assets/*.gif \
        --out-dir main/boards/AI-dual-eyed-doll/assets

Requires Pillow: python3 -m pip install pillow
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"DEA1"
VERSION = 1
HEADER_SIZE = 16
DEFAULT_SIZE = 160
DEFAULT_DELAY_MS = 30


def _require_pillow():
    try:
        from PIL import Image  # noqa: F401
    except ImportError:  # pragma: no cover - dependency hint
        sys.exit("Pillow is required: python3 -m pip install pillow")


def load_frames(path: str):
    """Return (frames, delays) with frames as full RGB images, one per GIF frame.

    GIF transparent pixels are composited onto black to match the *_black asset
    naming, instead of letting Pillow turn them into an arbitrary palette entry.
    """
    from PIL import Image, ImageSequence

    with Image.open(path) as image:
        delays = []
        frames = []
        backdrop = Image.new("RGB", image.size, (0, 0, 0))
        for frame in ImageSequence.Iterator(image):
            delays.append(int(frame.info.get("duration", 0) or DEFAULT_DELAY_MS))
            rgba = frame.convert("RGBA")
            canvas = backdrop.copy()
            canvas.paste(rgba, (0, 0), rgba)
            frames.append(canvas)

    if not frames:
        raise ValueError(f"{path}: no frames decoded")

    return frames, delays


def content_box(frames):
    """Union of the non-black bounds across every frame, or None if all black."""
    from PIL import Image, ImageChops

    box = None
    for frame in frames:
        empty = Image.new("RGB", frame.size, (0, 0, 0))
        bounds = ImageChops.difference(frame, empty).getbbox()
        if bounds is None:
            continue
        if box is None:
            box = list(bounds)
        else:
            box[0] = min(box[0], bounds[0])
            box[1] = min(box[1], bounds[1])
            box[2] = max(box[2], bounds[2])
            box[3] = max(box[3], bounds[3])
    return tuple(box) if box is not None else None


def normalize_frames(frames, size: int, trim: bool):
    """Crop the artwork out of its black canvas and fill the panel with it.

    Nearest-neighbour is deliberate: the artwork is flat-shaded cartoon line art,
    so interpolated edges would only blur it, and smooth upscaling also wrecks the
    frame-to-frame delta (measured on cute_crop_160_black: 591KB nearest vs 1055KB
    bicubic vs 1165KB lanczos for the same 132 frames).
    """
    from PIL import Image, ImageOps

    if not trim:
        return [
            frame if frame.size == (size, size) else frame.resize((size, size), Image.NEAREST)
            for frame in frames
        ]

    box = content_box(frames)
    if box is None:
        raise ValueError("every frame is fully black, nothing to keep")

    cropped = [frame.crop(box) for frame in frames]
    # ImageOps.fit scales uniformly and centre-crops, so the art always fills the
    # panel without stretching it out of shape.
    return [ImageOps.fit(frame, (size, size), method=Image.NEAREST) for frame in cropped]


def rgb565_le(image) -> bytes:
    """Convert an RGB image to little-endian RGB565, the layout the board expects."""
    raw = image.convert("RGB").tobytes()
    out = bytearray(len(raw) // 3 * 2)
    for src in range(0, len(raw), 3):
        r = raw[src]
        g = raw[src + 1]
        b = raw[src + 2]
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        dst = src // 3 * 2
        out[dst] = value & 0xFF
        out[dst + 1] = value >> 8
    return bytes(out)


def delta_encode(frames):
    """Byte-wise (current - previous) & 0xFF, with an all-zero first basis."""
    deltas = []
    previous = bytes(len(frames[0]))
    for frame in frames:
        deltas.append(bytes(((cur - prev) & 0xFF) for cur, prev in zip(frame, previous)))
        previous = frame
    return deltas


def pack(frames, delay_ms: int, size: int) -> bytes:
    deltas = delta_encode(frames)
    compressed = [zlib.compress(delta, 9) for delta in deltas]

    header = MAGIC + struct.pack(
        "<HHHHHH",
        VERSION,
        len(compressed),
        size,
        size,
        delay_ms,
        0,
    )
    table = struct.pack(f"<{len(compressed)}I", *(len(c) for c in compressed))
    return header + table + b"".join(compressed)


def verify(blob: bytes, frames, size: int) -> None:
    """Decode the packed blob with the same rules as the firmware and compare."""
    if blob[:4] != MAGIC:
        raise AssertionError("magic mismatch")
    version, count, width, height, _delay, _reserved = struct.unpack_from("<HHHHHH", blob, 4)
    if version != VERSION or width != size or height != size:
        raise AssertionError("header mismatch")
    if count != len(frames):
        raise AssertionError(f"frame count {count} != {len(frames)}")

    table = struct.unpack_from(f"<{count}I", blob, HEADER_SIZE)
    offset = HEADER_SIZE + 4 * count
    accum = bytearray(len(frames[0]))
    for index in range(count):
        delta = zlib.decompress(blob[offset:offset + table[index]])
        offset += table[index]
        if len(delta) != len(accum):
            raise AssertionError(f"frame {index} delta length {len(delta)}")
        for i, byte in enumerate(delta):
            accum[i] = (accum[i] + byte) & 0xFF
        if bytes(accum) != frames[index]:
            raise AssertionError(f"frame {index} does not round-trip")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("gifs", nargs="+", help="input .gif files")
    parser.add_argument("--out-dir", required=True, help="directory for the generated .anim.z assets")
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE, help=f"square panel size (default {DEFAULT_SIZE})")
    parser.add_argument(
        "--delay-ms",
        type=int,
        default=DEFAULT_DELAY_MS,
        help=f"fallback frame delay when the GIF has none (default {DEFAULT_DELAY_MS})",
    )
    parser.add_argument(
        "--no-trim",
        action="store_true",
        help="keep the original canvas instead of cropping the black border and scaling to fill",
    )
    parser.add_argument("--no-verify", action="store_true", help="skip the round-trip decode check")
    args = parser.parse_args()

    _require_pillow()
    os.makedirs(args.out_dir, exist_ok=True)

    total = 0
    for path in args.gifs:
        raw_frames, delays = load_frames(path)
        frames = normalize_frames(raw_frames, args.size, trim=not args.no_trim)
        delay = min((d for d in delays if d > 0), default=args.delay_ms)

        packed = [rgb565_le(frame) for frame in frames]
        blob = pack(packed, delay, args.size)
        if not args.no_verify:
            verify(blob, packed, args.size)

        stem = os.path.splitext(os.path.basename(path))[0]
        # cute_crop_160_black -> cute.anim.z keeps the runtime name short and
        # stable no matter how the source GIF is named.
        name = stem.split("_crop")[0] + ".anim.z"
        out_path = os.path.join(args.out_dir, name)
        with open(out_path, "wb") as handle:
            handle.write(blob)

        total += len(blob)
        trim_note = ""
        if not args.no_trim:
            box = content_box(raw_frames)
            trim_note = f"  crop={box[2] - box[0]}x{box[3] - box[1]}@{box[0]},{box[1]}"
        print(
            f"{os.path.basename(path):<32} {len(frames):>4} frames  {delay:>3} ms  "
            f"{len(blob) / 1024:>8.1f} KB{trim_note}  ->  {name}"
        )

    print(f"total {total / 1024:.1f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
