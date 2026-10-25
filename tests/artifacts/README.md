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
clean.

## Rights

The screenshots under `../screenshots/` are frames rendered from free,
author-published or publisher-published trial data.  The artwork, text and fonts
in them remain the property of their authors; they are kept here as local test
evidence for this port, not for redistribution.
