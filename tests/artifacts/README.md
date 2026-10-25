# Test artefacts

Kept here so a claim in the docs has something to look at.  Two different kinds:

- `ipt-diff-selfmade.png` - built entirely by this tree's own fixtures: a real
  `.ipt` descriptor decoded against a white base and one tile the test
  generates.  No game artwork in it.  Reproduce with
  `python3 tests/gen_ipt_realdescriptor.py <real.ipt> <dir>` then
  `./build/iettool decode <dir>/<name>.ipt out.png`.

- `sfade-gain500.png`, `sfade-midfade-gain750.png` are **not** committed: they
  are halves/brightness-ramps of a background image belonging to the game, and
  the point of the check is the numbers, not the picture.  Regenerate either
  with:

  ```
  ./build/fxtool sfade ../testdata/artemis-nicolan/extracted/bgimage/main.png \
      /tmp/sfade.png --gain 500
  ```

  The assertions that matter are in `./build/fxtool selftest`:
  mean RGB 31.38/46.48/95.74 -> 15.94/23.50/48.12, alpha 255 throughout,
  R range 0..255 -> 0..128, 714137/714752 pixels changed.

`fx-mutations.py` is the script that breaks each module on purpose and checks
the self test notices; it rewrites source files, so run it only when the tree is
clean.  It currently covers 11 breakages (the original seven plus colortone's
clamp, extrans' rounding, excall's return and savetitle's byte-vs-code-point
truncation); every one is caught.

## Second batch: colortone / extrans / excall / tags / guistate

These five modules are pure logic, so no new pixels are committed here.  Each
has a command that runs on the real corpus or on a synthetic image:

```
./build/fxtool tags                 # .ast census: colortone 141, extrans 34,
                                    # excall 56, cacheclear 292, exfont 266,
                                    # savetitle 55, movie 5, ...
./build/fxtool excall               # d09_hy01_1 -> ... -> d09_hy03, depth 6;
                                    # unresolved targets are listed, no dialogue
./build/fxtool excall d01_c01_3     # the real nested d01_c01_3 -> d01_c01_4 pair
./build/fxtool guistate             # the 16 scetween slots, 8 marked real
./build/fxtool colortone --bench    # 1024x698 sepia time (about 2.4 ms/frame)
./build/fxtool colortone tests/artifacts/ipt-diff-selfmade.png /tmp/sepia.png \
    --style sepia
./build/fxtool extrans tests/artifacts/ipt-diff-selfmade.png /tmp/sepia.png \
    /tmp/half.png --time 1000 --at 500
```

The pixel assertions live in `./build/fxtool selftest` (337 passed): colortone's
Q10 matrix, clamp and identity; extrans' t=0 / mid / end values and the bare
`{"extrans"}` instant switch; excall's A -> B -> C return chain plus the real
six-deep chain; savetitle's UTF-8 boundary; and the texture cache's freed count.


## Rights

The screenshots under `../screenshots/` are frames rendered from free,
author-published or publisher-published trial data.  The artwork, text and fonts
in them remain the property of their authors; they are kept here as local test
evidence for this port, not for redistribution.
