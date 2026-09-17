#!/usr/bin/env python3
"""Pack 160x160 GIF animations into the DualEyeDisplay frame-delta asset format.

The AI-dual-eyed-doll board has 4MB flash, so a full RGB565 frame (51,200 bytes)
per animation frame is far too expensive to store raw. Instead each frame is
stored as a byte-wise delta against the previous frame:

    delta[i] = (frame[i] - previous[i]) & 0xFF

Cartoon eye animations are mostly static between frames, so the delta is almost
all zeros and zlib collapses it. Measured on the shipped assets (160x160, 30ms,
full frame rate):

    name     frames    raw rgb565 + zlib    delta + zlib
    cute        132             1,160 KB         592 KB
    happy        68               523 KB         154 KB
    sad         144             1,052 KB         357 KB
    scared      138             1,446 KB       1,262 KB

scared is by far the worst case: its iris shimmer changes a quarter of the panel
every frame, so it costs twice as much as sad for the same frame count.

Two things matter a lot for the resulting size:

  * Scale with NEAREST, never a smoothing filter. The artwork is flat-shaded
    cartoon line art; LANCZOS/BICUBIC turn its edges into gradients whose noise
    differs every frame, which destroys the frame-to-frame delta (measured on
    cute: 592KB NEAREST vs 1055KB bicubic).
  * Pack the untrimmed source GIFs, not a pre-upscaled derivative. Feeding this
    packer the LANCZOS-upscaled files from Pictures/动图/optimized_160 makes the
    four animations total 3895KB instead of 2365KB.

The source GIFs draw their artwork inside a smaller centred square on a black
canvas (124x124 inside 160x160 for the shipped set), which shows up as a black
ring around the eyes on the panel. By default the packer takes the union of the
non-black bounds across every frame, crops to it, and scales the result back up
to the panel size so the artwork fills the screen. Pass --no-trim to keep the
original canvas.

Use --frame-step to trade smoothness for flash. The AI-dual-eyed-doll eye
animations live in a 1MB assets partition, and the shipped 30ms clips do not fit
there, so they are packed with every third frame kept (--frame-step 3), which
gives 90ms per frame, 11fps and roughly a third of the bytes.

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
        "C:/Users/YuYevi/Pictures/动图/cute_crop_160_black.gif" \
        "C:/Users/YuYevi/Pictures/动图/happy_crop_160_black.gif" \
        "C:/Users/YuYevi/Pictures/动图/sad_crop_160_black.gif" \
        "C:/Users/YuYevi/Pictures/动图/scared_crop_160_black.gif" \
        --out-dir main/boards/AI-dual-eyed-doll/partition_assets \
        --frame-step 3

The output directory is packed verbatim into the read-only "assets" partition by
spiffs_create_partition_assets() in main/CMakeLists.txt, so it must contain the
generated *.anim.z files and nothing else.

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


def resample_timeline(frames, delays, step_ms):
    """Spread a variable-delay GIF onto a uniform grid of step_ms frames.

    The packed format carries a single delay for the whole clip, so a GIF whose
    frames have mixed durations (a long hold plus a quick blink, say) cannot be
    stored as-is: keeping the shortest delay per frame collapses a six-second
    blink cycle into under a second. Sampling the timeline instead reproduces the
    real cadence - the long hold simply becomes many copies of the same frame,
    and a repeated frame is an all-zero delta that zlib collapses to a few bytes.

    Returns (frames, step_ms, source_duration_ms).
    """
    if step_ms <= 0:
        raise ValueError("step_ms must be positive")

    starts = []
    cursor = 0
    for delay in delays:
        starts.append(cursor)
        cursor += max(1, int(delay))
    total_ms = cursor

    count = max(1, int(round(total_ms / step_ms)))
    sampled = []
    for tick in range(count):
        at_ms = tick * step_ms
        index = 0
        for frame_index, start in enumerate(starts):
            if at_ms >= start:
                index = frame_index
            else:
                break
        sampled.append(frames[index])
    return sampled, step_ms, total_ms


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
    parser.add_argument(
        "--frame-step",
        type=int,
        default=1,
        metavar="N",
        help="keep every Nth frame and stretch the frame delay by N (default 1, i.e. keep all)",
    )
    parser.add_argument(
        "--resample-ms",
        type=int,
        default=0,
        metavar="N",
        help="rebuild the clip on a uniform N ms grid instead of using one GIF frame "
             "per packed frame; preserves the real cadence of GIFs whose frames have "
             "mixed durations (0 disables it, the default). --frame-step is then ignored",
    )
    parser.add_argument("--no-verify", action="store_true", help="skip the round-trip decode check")
    args = parser.parse_args()

    if args.frame_step < 1:
        parser.error("--frame-step must be at least 1")

    _require_pillow()
    os.makedirs(args.out_dir, exist_ok=True)

    total = 0
    for path in args.gifs:
        raw_frames, delays = load_frames(path)
        frames = normalize_frames(raw_frames, args.size, trim=not args.no_trim)
        if args.resample_ms > 0:
            # Mixed-duration GIFs: keep the real timeline, not the frame list.
            frames, delay, source_ms = resample_timeline(frames, delays, args.resample_ms)
            timing = f"  source={source_ms} ms loop={len(frames) * delay} ms"
        else:
            if args.frame_step > 1:
                frames = frames[:: args.frame_step]
            # The packed format carries a single delay for the whole clip, so the
            # shortest per-frame delay is the honest one to keep.
            delay = min((d for d in delays if d > 0), default=args.delay_ms) * args.frame_step
            timing = ""

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
            f"{os.path.basename(path):<32} {len(frames):>4} frames  {delay:>4} ms "
            f"({1000.0 / delay:>4.1f} fps)  "
            f"{len(blob) / 1024:>8.1f} KB{trim_note}{timing}  ->  {name}"
        )

    print(f"total {total / 1024:.1f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
