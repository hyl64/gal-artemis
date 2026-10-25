#!/usr/bin/env python3
"""Write our own tiles for a real composite descriptor, so it can be decoded
without touching any game pixel.

The henpri corpus ships the `.ipt` descriptors but not their images.  This reads
one descriptor, builds a white base at the w/h the descriptor declares, builds
one semi-transparent tile per `file = "..."` entry, and copies the descriptor
text beside them.  The result decodes exactly like the shipping file, but every
pixel in it was generated here.

    python3 tests/gen_ipt_realdescriptor.py \\
        ../testdata/artemis-henpri-trial/extracted/ipt_descriptors/ev_chi_01/ev_chi_01_02.ipt \\
        /tmp/ipt_realdescriptor
    ./build/iettool decode /tmp/ipt_realdescriptor/ev_chi_01_02.ipt out.png
    ./build/iettool decode --archive <archive.pfs> <member.ipt> out.png
"""

import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # importing the bench helper must not leave __pycache__
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_ipt_bench import write_png  # noqa: E402


BASE_RE = re.compile(
    r'base\s*=\s*\{\s*"([^"]+)"[^}]*?\bw\s*=\s*(\d+)[^}]*?\bh\s*=\s*(\d+)')
TILE_RE = re.compile(r'file\s*=\s*"([^"]+)"')


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("descriptor", help="a real composite .ipt")
    parser.add_argument("directory", help="where to write the synthetic tiles")
    parser.add_argument("--tile-width", type=int, default=100)
    parser.add_argument("--tile-height", type=int, default=80)
    args = parser.parse_args()

    text = Path(args.descriptor).read_text(encoding="utf-8", errors="surrogateescape")
    base = BASE_RE.search(text)
    if base is None:
        print("no `base = { \"name\", ..., w=, h= }` in that descriptor", file=sys.stderr)
        return 1
    base_name, width, height = base.group(1), int(base.group(2)), int(base.group(3))
    tiles = TILE_RE.findall(text)

    out = Path(args.directory)
    out.mkdir(parents=True, exist_ok=True)
    # an opaque white base, exactly the canvas size the descriptor declares
    white = bytes([255, 255, 255, 255]) * (width * height)
    write_png(out / f"{base_name}.png", width, height, white, alpha=True)
    # one semi-transparent tile each, a different colour per tile
    for index, name in enumerate(tiles):
        colour = bytes([(10 + 20 * index) & 0xFF, 200, 30, 128])
        pixels = colour * (args.tile_width * args.tile_height)
        write_png(out / f"{name}.png", args.tile_width, args.tile_height,
                  pixels, alpha=True)
    (out / Path(args.descriptor).name).write_text(text, encoding="utf-8",
                                                  errors="surrogateescape")

    print(f"{out / Path(args.descriptor).name}: base {base_name} {width}x{height}, "
          f"{len(tiles)} tile(s) at {args.tile_width}x{args.tile_height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
