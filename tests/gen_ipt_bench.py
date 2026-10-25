#!/usr/bin/env python3
"""Build a synthetic 1920x1080 composite fixture for timing `iettool decode`.

No game data is involved: the base and every tile are generated here with
deterministic pseudo-random pixels (incompressible enough that libpng has to do
real work), then a normal `mode = "diff"` descriptor is written beside them.

    python3 tests/gen_ipt_bench.py /tmp/artemis_ipt_bench
    /usr/bin/time -l ./build/iettool decode /tmp/artemis_ipt_bench/bench.ipt \
        /tmp/artemis_ipt_bench/out.png

The same directory can be turned into an archive with `pfstool create`, which is
how the "no whole-archive decryption" memory check is set up:

    ./build/pfstool create /tmp/artemis_ipt_bench/bench.pfs.002 \
        'image\\ev\\bench\\bench.ipt=/tmp/artemis_ipt_bench/bench.ipt' ...
"""

import argparse
import random
import struct
import sys
import zlib
from pathlib import Path


def chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: Path, width: int, height: int, pixels: bytes, alpha: bool) -> None:
    """8-bit RGBA (or RGB) PNG with one filter-0 row per scanline."""
    channels = 4 if alpha else 3
    stride = width * channels
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", ihdr)
                     + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
                     + chunk(b"IEND", b""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="where to write the fixture")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--tile-width", type=int, default=240)
    parser.add_argument("--tile-height", type=int, default=135)
    parser.add_argument("--columns", type=int, default=8)
    parser.add_argument("--rows", type=int, default=5)
    parser.add_argument("--seed", type=int, default=20251025)
    args = parser.parse_args()

    out = Path(args.directory)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    # an opaque base, random so its 8.3 MB really decompresses
    base = bytearray(rng.randbytes(args.width * args.height * 4))
    for at in range(3, len(base), 4):
        base[at] = 0xFF
    write_png(out / "bench_base.png", args.width, args.height, bytes(base), True)

    tile_count = args.columns * args.rows
    # semi-transparent tiles with random colour: the blend loop runs on every pixel
    tile = bytearray(rng.randbytes(args.tile_width * args.tile_height * 4))
    for at in range(3, len(tile), 4):
        tile[at] = 128
    for index in range(tile_count):
        write_png(out / f"t{index:02d}.png", args.tile_width, args.tile_height,
                  bytes(tile), True)
        # rotate the colours a little per tile so the PNGs are not identical
        tile = tile[4:] + tile[:4]

    step_x = args.width // args.columns
    step_y = args.height // args.rows
    lines = [
        "ipt = {",
        '\tmode = "diff",',
        '\tbase = { "bench_base", x=0, y=0, w=%d, h=%d, ax=%d, ay=%d },'
        % (args.width, args.height, args.width // 2, args.height // 2),
    ]
    for index in range(tile_count):
        column = index % args.columns
        row = index // args.columns
        lines.append('\t{ id=%d, file="t%02d", x=%d, y=%d },'
                     % (index + 1, index, column * step_x, row * step_y))
    lines.append("}")
    (out / "bench.ipt").write_text("\n".join(lines) + "\n")

    total = sum(p.stat().st_size for p in out.glob("*.png"))
    print(f"{out}/bench.ipt: {args.width}x{args.height}, {tile_count} tiles "
          f"({args.tile_width}x{args.tile_height}), png bytes {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
