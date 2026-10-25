# Real Artemis data: what this tree reads, and what it does not

Every claim below was produced by a command in this repository, run on data that
is not a fixture.  The samples are in `../testdata/` (outside this tree's write
scope; they are the scout's deliverables, not ours):

| sample | shape | size |
|---|---|---|
| `testdata/artemis-nicolan/root.pfs` | pf6, **plaintext**, bare `root.pfs`, 125 entries | 44,161,036 B |
| `testdata/artemis-henpri-trial/raw/HENPRI.pfs.060` | pf8, **keyed**, PC volume `*.pfs.NNN`, 1682 entries | 211,602,406 B |
| `testdata/artemis-raspberry-web/samples/**` | samples of a 212 MB pf8 Web release (full archive not kept) | - |
| `testdata/artemis-henpri-trial/extracted/**` | reference extraction of the pf8 volume, for byte comparison | - |

This file is written to stay honest: a capability that is only fixture-tested, or
only sniffed, or not done at all, is listed as such and not as compatibility.

## 1. Verified on real data

### 1.1 The archive container, all three shapes

```
$ ./build/pfstool info testdata/artemis-nicolan/root.pfs
magic       pf6 (version 6)
index       5728 byte(s), 125 entries
key         none (content is plaintext)
key phase   none
backing     memory map (streaming)
  sample  ttf        GenShin-Mono-Normal.ttf      TrueType font
  sample  ttf        GenShinGothic-P-Medium.ttf   TrueType font
  sample  png        bgimage\futari.png           PNG image
  sample  jpeg       bgimage\hoshi.jpg            JPEG image
```

The pf6 release is plaintext and its members begin with valid signatures - the
"pf6 is encrypted" reading was a mid-download artefact, not the format.

```
$ ./build/pfstool info testdata/artemis-henpri-trial/raw/HENPRI.pfs.060
magic       pf8 (version 8)
index       94293 byte(s), 1682 entries
key         SHA1(index), 20 bytes
key phase   entry  (content score 1200)
backing     memory map (streaming)
  sample  png        image\bg\bg50a.png           PNG image
```

`content score 1200` is the maximum: every sampled member starts with a real
signature under the entry-relative key rule.  The rule was found by the reader
itself (`src/pfs.cpp` tries both and scores them), not supplied by us.

Byte-exact extraction against the reference copy:

```
$ ./build/pfstool cat testdata/artemis-henpri-trial/raw/HENPRI.pfs.060 'script\d01_c01_2.ast' > /tmp/x.ast
$ cmp /tmp/x.ast testdata/artemis-henpri-trial/extracted/script/d01_c01_2.ast
$ shasum -a 256 /tmp/x.ast
33fc3e9d009844f7d6b7c18b...  (identical, 40,291 B)
```

A real PNG extracted the same way passes its own CRCs:

```
$ ./build/pfstool cat ... 'image\bg\bg50a.png' > /tmp/bg.png
IHDR 1600 x 905 bitdepth 8 colortype 6; 142 chunks; bad crc 0
```

**Performance**: the 211 MB volume is memory-mapped; `pfstool list` parses the
94,293-byte index and reads no content.  `ExtractTo` decrypts a 256 KiB window at
a time.

### 1.2 Content-based typing (never by extension)

`artemis::SniffBytes` decides from the first 512 bytes; the real corpus makes
this necessary, because the same extension means different things:

- `.iet` is a **KAG tag script** in ニコラン (Shift-JIS text with `*label` and
  `[tag]`), and an image-descriptor elsewhere.  `sniff` calls it `kag-iet`.
- `.ipt` is four different things (below).
- `.ast` is a Lua table, `.asb` is bytecode, `.psm`-style binary is absent here.

Typing covers `png jpeg ogg wav ttf otf psb pfs asb kag-iet ast-lua ipt-* ini sli
lua table-text`.  Three corpora, three kinds of evidence - a full pf6 archive, a
full pf8 volume, and a corpus that ships only loose samples:

```
$ ./build/pfstool list --types testdata/artemis-nicolan/root.pfs | head -6
root.pfs: pf6, 125 file(s), key phase none (plaintext)
    33615507     4978216  ttf        GenShin-Mono-Normal.ttf
    38593723     5042148  ttf        GenShinGothic-P-Medium.ttf
        5735       17120  text       backlog.iet
       22855       36937  png        bgimage\futari.png
       59792       43347  jpeg       bgimage\hoshi.jpg
```

```
$ ./build/pfstool list --types testdata/artemis-henpri-trial/raw/HENPRI.pfs.060 | head -3
HENPRI.pfs.060: pf8, 1682 file(s), key phase entry
       94300     1144948  png        image\bg\bg50a.png
     1239248        7790  png        image\bg\pink.png
$ ... | grep 'ev_chi_01'
    37531230         156  ipt-composite image\ev\ev_chi_01\zev_chi_01_02.ipt
    37531386       27462  png        image\ev\ev_chi_01\zev_chi_01_02.png
```

The raspberry corpus keeps no archive for this tree (212 MB was not stored; only
`archive_index.txt` and extracted samples), so the third evidence is the typing
of loose files - `pfstool type`, one file at a time:

```
$ ./build/pfstool type testdata/artemis-raspberry-web/samples/*.ast \
      .../samples/*.iet .../samples/*.ini .../samples/*.lua \
      .../samples/asb/*.asb .../samples/ipt/*.ipt
script_オープニング続き.ast   ast-lua        score 700   Artemis .ast script (Lua table)
system_first.iet              kag-iet        score 700   KAG tag script (Shift-JIS or UTF-8)
system.ini                    ini            score 700   INI configuration
system_ui_title.lua           lua            score 600   Lua system script
system_save.asb               asb            score 1200  ASB bytecode, 12 items
system_script.asb             asb            score 1200  ASB bytecode, 173 items
system_ui.asb                 asb            score 1200  ASB bytecode, 226 items
image_hd_anime_line1.ipt      ipt-anime      score 700   IPT animation descriptor
psw_ui_ja_mw_dialog.ipt       ipt-atlas      score 680   IPT sprite-sheet rectangles
psw_ui_ja_mw_map.ipt          ipt-data       score 680   IPT point/route table
psw_ui_ja_save_char.ipt       ipt-atlas      score 680   IPT sprite-sheet rectangles
```

Nothing there is decided by the extension: `.iet` is a script, `.ipt` splits into
two dialects, and `.ini` is only recognised after skipping its `;` banner (the
synthetic signature table in `pfstool selftest` covers the formats; these three
runs cover the real bytes).

### 1.3 `.asb`, the binary script: three real samples, byte-exact

```
$ ./build/asbtool selftest
  artemis-raspberry-web/samples/asb/system_save.asb:   12 item(s),  2 label(s)  ok
  artemis-raspberry-web/samples/asb/system_script.asb: 173 item(s), 59 label(s) ok
  artemis-raspberry-web/samples/asb/system_ui.asb:     226 item(s), 45 label(s) ok
all passed
```

All three files parse to their declared item count with no truncation, which is
the end-to-end check (the string lengths exclude the NUL and a wrong offset makes
the walk fail immediately).  The dump:

```
$ ./build/asbtool dump .../system_ui.asb 5
*calltag
wt
tag              data=$t.tagtext
return
*logo
226 item(s) read, 226 declared
```

Attribute names can be numbers and values can be `$` expressions; both are kept
as strings and never evaluated.

### 1.4 `.ast`, the Lua-table script: both real variants, through the CLI

`dump` decides from the bytes which reader to use, so the command the
independent verifier runs is the one that works - not a library path beside it:

```
$ ./build/asttool dump testdata/artemis-henpri-trial/extracted/script/d01_c01_2.ast
... chained layout astver 2.0, 78 block(s), 287 command(s), 77 line(s), 55 voice ref(s)
  77 block(s) carry a linknext
  first 20 of 77 spoken line(s):
    block_00000
      [モブ男B,怖い警官] 「お前がやったんだろう！？」
    block_00001
      叩きつけられた腕で、灰色の机が震える。
```

```
$ ./build/asttool dump testdata/artemis-henpri-trial/extracted/script/d05_g02_3_5.ast
... chained layout astver 2.0, 310 block(s), 980 command(s), 309 line(s), 135 voice ref(s)
    block_00000
      [夕顔] 「0720番が、独房から消えた？」
```

This file is the `[[...]]` + `{"ruby", text=...}` mixed sample: 309 lines come
out, long brackets and inline ruby readings included.

```
$ ./build/asttool dump testdata/artemis-raspberry-web/samples/script_オープニング続き.ast
... arrayed (dialogue external) layout, 54 block(s), 129 command(s), 0 line(s)
  note: 54 block(s) keep their dialogue in an external lang_NNNNN file
```

The B layout's dialogue is in `lang_NNNNN` files that this corpus does not ship,
so zero lines is correct and the external `lang` name is recorded.

`asttool text` on the same file prints the lines with their speakers and blocks:

```
$ ./build/asttool text .../d01_c01_2.ast | head -3
...: 77 line(s)
  | [モブ男B,怖い警官] 「お前がやったんだろう！？」   (block_00000)
  | 叩きつけられた腕で、灰色の机が震える。   (block_00001)
```

`asttool selftest` covers these three files **as child processes**
(`asttool dump <file>` / `asttool text <file>` must exit 0 and print the expected
content), because a self test that calls the parser directly cannot notice a
subcommand still wired to the old reader - which is exactly what happened once.

Both real layouts parse: the chained one (`astver = 2.0` + `block_NNNNN` +
`linknext` + inline `text.ja`) and the arrayed one (top-level array + `lang`
pointing at an external language file).  The shared parser handles what the real
59 scripts use: floats, negative numbers, `true`, `[600] = {...}` bracketed keys,
and `[[...]]` long strings (needed because dialogue contains half-width quotes).
The self test parses the real samples when they are present, and both do.

`asttool selftest` is fully green, including fixtures shaped like the corpus.

### 1.5 `.ipt`: the four real dialects are told apart

```
$ ./build/iettool info .../samples/ipt/psw_ui_ja_save_char.ipt
  dialect   atlas      rects 4   (瑠莉 0,1,166,93 / 美琴 0,97,166,93 / ...)
$ ./build/iettool info .../samples/ipt/psw_ui_ja_mw_dialog.ipt
  dialect   atlas      rects 17  (ok_qs 576,0,424,36 / qload 576,36,424,36 / ...)
$ ./build/iettool info .../samples/ipt/psw_ui_ja_mw_map.ipt
  dialect   data       points 8 with x/y
$ ./build/iettool info .../samples/ipt/image_hd_anime_line1.ipt
  dialect   anime      mode anime_full, canvas 1280 x 720, frames line11 line12 line13
```

Bracketed Japanese keys (`["瑠莉"]`), comments, and `{x=50, y=394}` tables all
parse.  The **composite** dialect (`mode = "cut"|"diff"`) is implemented and
pixel-verified, but **only against fixtures**: the four real `.ipt` samples in
this corpus are atlas/data/anime, and the composite descriptors that exist
(henpri `image/ev/**`) were not rendered.

## 2. Not done

These are gaps, not footnotes.

1. **`.iet` as a KAG script is only sniffed, never parsed.**  ニコラン's 16 `.iet`
   files are Shift-JIS KAG text (`*label`, `[bgm file="moon"]`,
   `[lyc id="0" file="bgimage/milkeyway"]`, `[trans ...]`, dialogue with `@[rp]`).
   `sniff` recognises them as `kag-iet`; nothing tokenises tags or executes them,
   so **no real first screen was rendered** and `agame` still draws only its own
   fixture.  This is the largest single gap, and it is why "the engine can be
   played" is not claimed anywhere.
2. **No real composite IPT was decoded.**  The compositor, its blend rule, its
   clipping and its diff-mode base handling are pinned by `iettool selftest`
   (tiles beside the descriptor, tiles inside an `archive.pfs`, transparent and
   non-alpha tiles, negative origins, missing tiles) - all fixture pixels.  The
   composite descriptors in the corpus were not run through it.
3. **`.ipt` animation is parsed, not played.**  `anime_full` frames, `time`,
   `max` and `loop` are read; no frame is sequenced, and no animation is drawn.
4. **`.psb` is not implemented.**  The E-mote PSB container
   (`reference/ArcPSB.cs`, tag-serialised PSB with an encrypted header and a
   names trie) has **no sample in any of the three corpora**: the titles here
   ship standing art as `fg` commands over PNG, not as `.psb`.  `sniff` recognises
   the `PSB\0` signature and reports its version; nothing reads its tags.  Had
   there been a sample, the header path would have been worth writing; instead
   this is documented as absent, not as working.
5. **Real `.ast` is read, not written.**  `asttool dump`/`roundtrip` still operate
   on the invented text format this tree started with; the real Lua-table layout
   has a reader (`asttool real`) and no writer, by decision
   (see the stop-loss note in section 4).
6. **No script semantics.**  Commands are extracted as name + attributes, but
   nothing evaluates `[if]` conditions, expands macros, resolves expressions
   (`$t.release == 1`), implements the Lua host API (`get_eval`, `e:now()`), or
   drives a runtime VM.  `.asb` bytecode is decoded structurally, not executed.
7. **Voice lines stay external.**  The arrayed `.ast` variant points at
   `lang_NNNNN` files; this tree records the file name and does not open it, so
   those blocks show zero lines.
8. **A lesson kept on purpose: test the entry point users run, not the library
   beside it.**  `asttool selftest` was green for hours while
   `asttool dump <real .ast>` failed, because the self test called the new parser
   directly and the subcommand was still wired to the old reader.  The real
   scripts are now checked by running `asttool dump <file>` and
   `asttool text <file>` as child processes and asserting on their output.  Any
   capability added here needs the same shape of test: the CLI is the product.
9. **No audio, no save data.**  `.ogg`/`.sli` (sound loop points) are sniffed and
   never played or applied.  `.asb` save/load commands are decoded but there is no
   save-file format work.
9. **Multi-volume games are read, not cross-searched.**  Each `.pfs.NNN` is an
   independent archive and is read as one; nothing resolves a name across a whole
   game folder yet, and only one real volume was exercised.
10. **`.psb`-free Windows/PC-only features** (encryption variants beyond the
    SHA1 rule, other engine generations) are untested because no such sample
    exists here.

## 3. What the real data corrected

- The content key runs from **key offset 0 per entry**, not from the entry's
  archive offset.  A 211 MB real pf8 archive decides it (TTF header decode).
- `pf6` is **plaintext**; the "fixed SHA1 key" assumption of the first writer was
  wrong for two of the three real shapes.
- **The extension lies.**  `.iet` = KAG script here, `.ipt` = four dialects,
  `.ast` = Lua tables (not the invented text form), `.asb` = bytecode.
- Member names use **backslashes**, and a release may be a **bare `root.pfs`**
  with no `.pfs.NNN` volumes.
- `.psb` standing art is a PC-title concern; the Web releases here use PNG + `fg`
  commands instead.

## 4. Reproducing all of it

```bash
cd artemis && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
./build/pfstool selftest      # container, every version and key rule
./build/asttool selftest      # old text format + real Lua layouts + real samples
./build/iettool selftest      # four dialects + composite pixels + archive tiles
./build/asbtool selftest      # ASB fixture + the three real samples
./build/arun --selftest       # the fixture runtime
# real archives:
./build/pfstool info  ../testdata/artemis-nicolan/root.pfs
./build/pfstool info  ../testdata/artemis-henpri-trial/raw/HENPRI.pfs.060
./build/asttool real  ../testdata/artemis-henpri-trial/extracted/script/d01_c01_2.ast
./build/asbtool dump  ../testdata/artemis-raspberry-web/samples/asb/system_ui.asb
./build/iettool info  ../testdata/artemis-raspberry-web/samples/ipt/psw_ui_ja_mw_map.ipt
```

Self tests look for real samples through the `ARTEMIS_TESTDATA` compile
definition (the repository's `testdata/` from the build), so they cover real
bytes where the corpus is present and say `skipped (not present)` where it is
not - never silently.

## 5. Scope handover

Mid-round the write scope was split because teammates were added in parallel.
The layers of this tree now divide as follows, and this file records the state
each one is in:

- **this task** (`src/pfs.*`, `src/sniff.*`, `src/lua*.*`, `src/ast.*`,
  `src/asb.*`, `tools/pfstool.cpp`, `tools/asttool.cpp`, `tools/asbtool.cpp`,
  `docs/`): container, content typing, real `.ast`, `.asb`.
- **teammate `ipt`** (`src/ipt.*`, `src/pngio.*`, `tools/iettool.cpp`): taken over
  after this task left them green.  Everything recorded in section 1.5 was
  verified before the handover; the composite-IPT fixture pixels in
  `iettool selftest` were written by this task.
- **teammate `screen`** (`tools/agame.cpp`, `tools/arun.cpp`, `src/runtime.*`,
  `src/iet.*`, `src/render/`): the KAG `.iet` script layer and the real first
  screen - the two capabilities this task did not build.
- **teammate `perf`**: performance work.

**Stop-loss note.**  This work was time-boxed to converge the tree to green
rather than to widen coverage.  Section 2 is the price of that: KAG script
execution, the real first screen, `.psb`, and composite-IPT-on-real-data were cut
and are recorded as missing, not as done.

## 6. IPT, finished (teammate `ipt`)

Section 1.5 records the four dialects being told apart.  This section records
what was built on top of that and how each claim can be falsified.

Two statements in the earlier brief were wrong and are corrected here: the atlas
descriptor `psw\ui\ja\mw\dialog.ipt` holds **17** rectangles (the 18th `=`
is the outer `ipt = {`), and a `diff` composite draws its base at **(0,0)** -
`base.x` / `base.y` are the canvas origin in world coordinates (the reference
implementation stores them as `ImageMetaData.OffsetX/OffsetY`), not an offset
inside the canvas.  Real geometry agrees: the base of `ev_chi_01_02.ipt` is
1668x998, exactly the canvas, and its tile sits at `(773,244)` inside it.

### 6.1 Self test: 136 checks, and how they were made falsifiable

```
$ ./build/iettool selftest
136 checks, 0 failed
$ ./build/pfstool selftest   40 checks      ./build/asttool selftest  22 checks
$ ./build/asbtool selftest   11 checks
```

The first version of this self test reported "all passed" while skipping every
real-data assertion, because `Check()` returns 0 for success and the fixture was
written as `if (!Check(...)) return;` - which returns on success.  That is the
same class of false green as the CLI/library split recorded in section 2, and it
is why the checks now run 136 instead of 73.

Four mutations were then used to prove the assertions can fail.  Each was forced
through a clean object rebuild, run, then reverted with the source hash checked:

| mutation | result |
|---|---|
| draw the base at `(base.x, base.y)` | exit 1, 3 red |
| write `alpha = sa` on a diff canvas | exit 1, 2 red |
| shift a negative-origin tile instead of clipping | exit 1, 2 red |
| order tiles by `id` instead of file order | exit 1, 3 red |

### 6.2 Composite pixels, with tiles this repository builds itself

The R18 corpus keeps only descriptors here, so every pixel assertion uses tiles
generated from the descriptor's own `file` and `w`/`h`.  One artefact is kept:

`tests/artifacts/ipt-diff-selfmade.png` - 1668x998, RGB (no alpha channel),
2 colours, 7,231 bytes.  It is the real `ev_chi_01_02.ipt` decoded against a
white 1668x998 base and one 100x80 tile `(10,200,30,128)` at `(773,244)`; the
tile area reads `(132,227,142,255)`, its neighbours stay white, and nothing in
the frame is translucent.

Reproduce:

```
python3 tests/gen_ipt_realdescriptor.py <real.ipt> <dir>
./build/iettool decode <dir>/<name>.ipt out.png
```

### 6.3 Archive reads without whole-archive decryption

A 203 MiB synthetic pf8 archive holding four 48 MB unrelated members plus one
1920x1080 composite decodes in a median **0.330 s** at a peak RSS of **39.9 MiB**
- 19.7% of the archive - and its output hashes identically to decoding the same
composite from a plain directory (`0.310 s`, 28.7 MiB).  `pfstool info` reports
`backing memory map (streaming)`.

A real bug fell out of measuring this: `pngio.cpp`'s colour count used an
`unordered_set` whose bucket count degenerated on keys with alpha 255, taking
**46 s** on a 1920x1080 frame.  Sorting and calling `unique` does the same job in
**50 ms**, which is why the decode is 0.31 s rather than 48 s.

## 7. The render layer and the real first screen (teammate `screen`)

This section was added by the render task after it took the two capabilities
section 5 recorded as missing: KAG `.iet` execution and a real first screen.  It
is append-only; sections 1-5 above are unchanged.

### 7.1 What runs now

`agame --script <scene.iet> --archive <root.pfs>` runs the real release with
nothing unpacked:

1. **boot**: `first.iet` top level.  This is where the UI actually lives.  It
   sets the variables the rest reads, calls the game's own Lua, and defines the
   message layers.  The `[chgmsg id="adv"] [font ...]` block alone is the
   dialogue box: face `GenShin-Mono-Normal.ttf`, 36 pt, box (42, 460) 937x250,
   white ink, `0x121212` shadow, and `[chgmsg id="name"]` the name box at
   (42, 390) 38 pt centred.  None of those numbers exist in this tree's source -
   they are read out of the game at run time.
2. **macros**: `[calllua function="initializeMainInit"]` is answered by a stub
   that takes the `e:tag{...}` calls out of the game's own Lua and runs them.
   That is what registers `system/macro_effect.iet`, `system/macro_nicolan.iet`,
   `system/save_normal.iet` and `system/name.iet` (112 macros in total).  Every
   `*label` in a registered file becomes a tag.
3. **scene**: `scenario/scene.iet` from `*start`.

`[girl]`, `[boy]`, `[n]`, `[bg_b]`, `[adv]`, `[mwout]`, `[mwin]`, `[lycolor]`,
`[bg]` … are **macros, not engine tags**.  `[rp]` is an engine tag.

### 7.2 Reproduce

```sh
cd artemis
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
SDL_VIDEODRIVER=dummy ./build/agame \
  --script scenario/scene.iet \
  --archive ../testdata/artemis-nicolan/root.pfs \
  --frames screenshots --frame-prefix real-first-screen-p \
  --out screenshots/real-first-screen.png --pages 4 --fade-frames 3
./build/agame selftest
```

The screenshot lands in `artemis/screenshots/`: `real-first-screen.png` is the
first page that has both a real background and words, `real-first-screen-p001..p004.png`
are the four pages in order, `fade001..003.png` sample the `[trans time="2000"]`
between page 2 and page 3.  The `.ast` demonstration path is unchanged:
`SDL_VIDEODRIVER=dummy ./build/agame tests/sample-scene.ast --frames <dir>`
still writes twelve BMPs.

Measured on this machine (macOS arm64, shared with the other build jobs, so wall
times move by a factor of two; CPU time is stable):

```
canvas: 1024x698 (from the game's system.ini), fps=60
scripts parsed: macro/first/system/develop_tool/system/tag_filter/
                system/macro_effect/system/macro_nicolan/system/save_normal/
                system/name/scenario/scene
commands executed: 365 (boot: 188)      pages: 4
macro table: 112 macros, 14 expansions (12 distinct this scene used)
lua e:tag calls executed: 16            conditions: 39 (0 unreadable)
first-screen text pixels: 29216 (framebuffer diff), 29435 (glyph mask), glyphs=61
first-screen colours: 82147, background colours: 32336
font: GenShin-Mono-Normal.ttf 4978216 bytes
      "GenShinGothic-Monospace-Normal" sha1=137cd613104e15fcc44aea3b69c836c933789a64
first screen: screenshots/real-first-screen.png (read back 1024x698, 82147 colours,
              29216 text pixels)
boot: 7.5-50.5 ms      font load: 4.4-46.8 ms     image decode: 22-69 ms (6 images, 4 cache hits)
engine: process start -> first page composited: 50-138 ms
RENDER PER FRAME: 5.3-11.5 ms average (composite + CoreText, no disk)
CAPTURE ONE-OFF: ~265-430 ms (colour census 28 ms + PNG write 223-391 ms + read-back 41 ms)
peak RSS: 53-59 MB
```

`agame selftest` holds seven cases green, and the real-data ones say `SKIP`
instead of failing when `testdata/artemis-nicolan` is absent:

```
PASS sjis-cp932        テスト　日本 / ①～㈱髙
PASS parse-scene.iet   labels=7 commands=1077 texts=458 inline=33
PASS boot-macros-styles macros=112 adv=36pt/GenShin-Mono-Normal.ttf/42,460 937x250
PASS first-screen      canvas=1024x698 text=29216 ink=29435 colours=82147 glyphs=61
                       speaker="少女" png=1024x698/82147 colours/29216 text px
PASS archive-vs-files  archive members and unpacked files agree byte for byte
PASS jpeg-background   bgimage/milkeyway.jpg 1024x768 90414 colours
PASS texture-cache     hits=1 misses=1
```

### 7.3 What the corpus taught, bug by bug

1. **CP932 is not `kCFStringEncodingShiftJIS`.**  CoreFoundation's Shift-JIS
   decoder rejects CP932 extensions: the bytes for `①`, `～` (0x8160), `㈱`, `髙`
   fail, so a whole page can decode to nothing.  The CP932 codec is
   `kCFStringEncodingDOSJapanese` (0x0420, "code page 932, also for Windows"),
   declared in `CFStringEncodingExt.h`.  With it the same bytes decode to
   `①～㈱髙`, matching `iconv -f CP932`.  `src/iet.cpp` uses it.
2. **Lua function bodies end with `end`, not `}`.**  The `[calllua]` stub first
   matched braces and therefore took the body of the *first* `e:tag{...}` call as
   the function body: it executed zero of the game's tag calls, no macro file was
   registered, `[girl]` was "unknown", and the name box and message window were
   never created.  Matching `function|if|for|while … end` fixed it (`vm.cpp`,
   `LuaBlockEnd`).  Note `do` must not be counted: it belongs to `for`/`while`.
3. **`macro.iet` is loaded by the engine, not by the game.**  `[adv]` - the ADV
   message-window setup - lives in the archive-root `macro.iet`, and the game's
   Lua has that `macroadd` commented out.  A boot that only honours the Lua gets
   no `[adv]`, no `[mwout]`, no `[mwin]`.  `Vm::Boot` adds `macro.iet` first,
   then runs the game's own macroadds; later files win on duplicate names.
4. **`[rp]` is an engine tag, and a page is the text up to `[rp]`.**  The real
   scene uses it 409 times.  A page with no text does not wait for the reader
   (that is how `system/name.iet` clears the name layer: `[chgmsg id="name"]
   [rp backlog="0"] ... [/chgmsg]`).  `@` inside the text breaks a line within
   the page.
5. **Layer sets move their children.**  `[button]` is itself a macro that draws
   `86.0.0` and `86.0.1` (idle/hover images), while the script positions
   `[lyprop id="86" left="870" top="645"]`.  Compositing children at their own
   coordinates put the menu button in the top-left corner; the child's position
   is the sum of its ancestors' offsets (`Stage::Composite`, `AncestorOffsets`).
6. **`lyprop alpha` is 0-255, and a fill layer has to honour it.**  The
   `$g.msgalpha` the boot sets is 200.  `[lycolor id="1" color="FFFFFF"]` plus
   `[lyprop id="1" alpha="100"]` is the tanabata scene's white veiling at 39%,
   not an opaque white screen.  The first compositor ignored a fill layer's
   alpha and the background vanished under it.
7. **An unset variable is zero in `[if estimate=...]`.**  `first.iet` runs its
   first-time block on `[if estimate="$g.firsttime == 0"]`; with a purely string
   comparison, an empty variable never equals `0`, the block is skipped,
   `g.msgalpha` stays unset, and every message layer is transparent.  Empty
   compares equal to 0; two non-numeric strings still compare as strings.
8. **`[trans]` is drawn at its final state.**  Time is not slept in a still-frame
   run; `--fade-frames N` samples the change from the previous page's picture to
   the new one.  Cost: three extra PNG writes for the captured page
   (`screenshots/fade001..003.png`), about 200 ms each at 1024x698.
9. **The stage size is the game's answer.**  `system.ini` says `WIDTH=1024
   HEIGHT=698 FPS=60`; the backgrounds are 1024x768 (the bottom 70 rows are
   cropped, as the engine does) and the portraits are 1024x1500 with a `top`
   offset (`[lyprop id="3" top="-400"]`) that selects the visible part.
10. **JPEG is not optional.**  `bgimage/milkeyway` - the tanabata background the
    first screen needs - is a JPEG, and so are six more backgrounds and the
    click-wait glyph.  PNG goes through `src/pngio.cpp`; JPEG (and WebP) through
    ImageIO's `CGImageSource`, a system framework, so no third-party decoder
    enters the tree.  Decode is by content, never by extension.
11. **The game's TTF must come from the game.**  `[font face="..."]` names
    `GenShin-Mono-Normal.ttf`; `CGFontCreateWithDataProvider` loads it from the
    archive member's bytes (no unpacking), `CTFramesetter` lays the dialogue out
    in the 937x250 box.  The self test prints the PostScript name and SHA-1 of
    the bytes used.  CoreText's bitmap context needs **no** CTM flip: a frame
    lays its first line out at the top of the path rect and the bitmap's first
    row is the top row.  The first version flipped it and every line came out
    upside down.

### 7.4 Compatibility, stated precisely

**Acted on** (has an effect on the picture or on control flow): `rp`, `print`,
`if`/`elseif`/`else`/`/if`, `jump`, `call`, `return`, `stop`/`exit`/`reset`,
`calllua`, `var` (including `system=` `os`, `screen_width`, `screen_height`,
`file_exists`, `var_exist`, `get_layer_info`, `delete`, `explode`),
`chgmsg`/`/chgmsg`, `font`, `macroadd`, `lyc`, `lyprop` (`alpha`, `top`, `left`,
`visible`), `lydel` (comma list included), `lycolor`, `trans` (recorded; final
state), `wait` (recorded, not slept), `flip` (no-op: the canvas is the frame),
inline `ruby`/`rb`/`rt` (furigana dropped, base text kept).

**Known, not acted on** (30 distinct, 145 occurrences in four pages).  Each one
is counted and reported; none is silently swallowed:

| tag | should do | now |
| --- | --- | --- |
| `scetween` (92) | **layer property tween, not text reveal.** `mode="init" type="..."` sets a GUI state slot's style; `mode="add" type="..." param="alpha\|top\|left\|xscale\|yscale\|rotate" ease="..." diff="<delta>" time="<ms>" delay="<ms>"` moves the layer by a **delta** (not to an absolute value) over `time` after `delay`. `type` is one of 15 state slots: show, hide, in, out, on, click, rollover, rollout, overwhite, overtrans, drag, dragout, backlog_up_in, backlog_up_out, backlog_down_in, backlog_down_out. `ease` is exactly five values: none (36, linear), easeout_sine (16), easeout_quad (4), easeout_quart (2), easeinout_quart (2). `delay` may be `$g.msgspeed`. All 92 live in the GUI scripts (first.iet 88 = 8 init + 80 add, backlog.iet 2, rclick.iet 2); **scene.iet has none**. | value curves implemented in `src/fx/`; not yet wired into the render loop |
| `anime` (21) | **two unrelated things under one name.** (a) the script tag `[anime id="82" mode="init" file=... top left alpha]` / `mode="add" ... time="<ms>"` / `mode="end" time="<ms>"]` is a **step keyframe track**: an `add` becomes visible after its `time` and then *stays* until the next one - `time` is a hold, not an interpolation (smooth motion is `lytween`). first.iet 18 = two glyph tracks (ids 82 and 81.1, adds arriving at 100,200,...,700 ms, 800 ms end hold) and scene.iet 3 = one background slide (`bgimage/run` -> `run2`, 500 ms + 1000 ms hold). (b) the IPT `mode="anime_full"` descriptor is a **frame sequence inside one file** (`time=100, max=3, loop=-1`, frames line11/line12/line13): 3 frames x 100 ms, `loop=-1` forever, `max` caps it at 3 cycles (900 ms), so the picture stops there. | frame selector implemented in `src/fx/`; not yet wired |
| `setonpush` (12) | key bindings | ignored (no input) |
| `lyevent` (3) | clickable layer events | ignored (no input) |
| `autosave` (2), `alreadyread` (1), `backlog` (1), `writebacklog` (1) | save/read UI state | ignored (no save system) |
| `lytween` (15), `lytweendel` (2) | absolute interpolation `id param from to ease time yoyo sync`. `yoyo="-1"` is a permanent ping-pong (the glyph blink); `yoyo="30"`/`"10"` are finite. A new `[lyc]` on the same layer implicitly deletes its tweens (the game's own comment in first.iet says so); `lytweendel` is the explicit form. **Three of the thirteen tags in scene.iet are broken**: `[lytween id="1" param="yscale" from="100" top="100" time="20000" sync="0"]` writes `top=` where `to=` belongs, so there is no destination - such tags must be refused and reported, never guessed. | parser implemented in `src/fx/` (refuses the broken ones); not yet wired |
| `glyph` (2) | click-wait mark homing to the text end | ignored (mark is not placed) |
| `callnative` (2) | browser/OS native call | ignored |
| `sfade` (14) | screen brightness ramp: reach `gain` in `time` ms, on a 0..1000 scale where 1000 leaves the picture untouched, 500 is half and 0 is black. Real gains: 0, 200 (x2), 300, 500 (x4), 1000 (x6). Measure: 1024x698 frame, integer `(v*gain+500)/1000` through a 256-entry LUT, alpha untouched, 0.62-0.75 ms per frame. | implemented in `src/fx/brightness`; not yet wired (still **a visible difference** on the tanabata scene) |
| `automode`, `rclick`, `hide`, `backlog`, `keyconfig` (each 1) | mode toggles | ignored |
| `scein`, `sceout` (each 1) | hide/show the message layer | ignored (layer visibility is still applied via `lyprop`) |
| `prohibit` (1), `indent` (1) | line-breaking rules for Japanese | ignored (CoreText breaks lines) |
| `caption` (1), `debug` (1) | window title / log level | ignored |
| `setonhidein`, `setonhideout`, `setonbacklogin`, `setonbacklogout` (each 1) | UI callbacks | ignored |
| `lyprop clickablethreshold` (2), `lyprop negative` (1), `lyprop grayscale` (1) | colour filters / hit area on a layer | ignored; reported as `lyprop <key>` |

**Unknown** (2, both counted with their first location): `splay`
(`system/macro_effect.iet:67`, the BGM play call; `src/audio/` now exists but the
tag is not wired yet) and `wasm_getos` (`first.iet:82`, a web-build-only tag).

**Two things the real corpus taught about this table** - both are traps:

1. **Text reveal is not a tag.** `[var name="g.msgspeed" data="30"]` is
   milliseconds per glyph, and the GUI script feeds the same value into
   `[scetween ... delay="$g.msgspeed"]`, so the window slides in slightly slower
   than the glyphs appear.  Read it as a variable, not as an animation tag.
2. **`@` must not be counted on Shift-JIS bytes.** The second byte of a cp932
   double-byte character is often `0x40`, which *is* `@`.  The corpus holds 717
   raw `@` bytes but only 697 real line breaks - the other 20 would be phantom
   breaks.  Pagination and reveal must run on decoded UTF-8 code points.

### 7.5 What this does not prove

The first screen is real: the game's archive, script, background, message
window, name window, font and text, composited at the game's own stage size and
verified from the framebuffer and again from the written PNG.  It is **not** a
playable game: audio (`bgm`/`seplay`/`splay`), save/load, choices (`[link]`,
`[select]`), animation (`anime`, `lytween`, `scetween`), screen effects
(`sfade`, `quake`), input and the title/menu screens are not implemented, and of
44 tag names used by the scene plus the macro files, 30 known ones are recorded
above as not acted on.  No claim should be made beyond "renders the real first
screen from the real data".

## 8. The runtime, assembled (teammate `integrate`)

Section 7 ended with a list of what the render layer did not do: audio, effects,
choices, input, and `.ast`.  This section is what changed.  Everything below was
run on the real data in `testdata/`; the shipped `.ast` corpus is an adult title,
so its frames go to `/tmp` and its words never enter this file - what is quoted
here is counts, hashes and pixel numbers.

### 8.1 `agame` reads a shipped `.ast` (the bug from `perf`'s report)

Two different languages wear the `.ast` suffix in this tree, and until now
`agame` only spoke the one this tree invented:

| | `tests/sample-scene.ast` | a shipped `.ast` |
|---|---|---|
| shape | `text = { "start" = { ... } }`, bare `[800]` markers | `astver = 2.0`, `ast = { block_NNNNN = { ... } }` |
| reader | `src/ast.cpp` | `src/luaast.cpp` |
| what `agame` did | 12 frames, the demonstration | `unexpected token at byte 17742` |

The reader is now chosen by **content, not extension**: `ParseLuaAst` is tried
first, and only when it refuses the file does the old reader take over.  That is
what keeps `tests/sample-scene.ast` producing exactly its twelve frames while a
real script plays.  `src/render/astscene.{h,cpp}` is the new half: the `linknext`
chain, the commands that change the picture, and the events (sound, effects) the
front end takes over.

**The chain is checked, not assumed.**  `WalkAstChain()` walks from the head and
reports a repeat, a dangling `linknext` and any block the walk never reached.
Over the whole corpus (`--ast-info <script/>`):

```
corpus.files=59  corpus.failed=0  corpus.chained=59  corpus.arrayed=0
corpus.blocks=13133  corpus.commands=41454  corpus.lines=13073
corpus.voices=5693  corpus.chain_defects=0
```

`chain_defects=0` is the assertion `agame selftest` makes: 13,133 blocks, every
one reached exactly once, no chain that loops and none that stops early.  For
`d01_c01_2.ast` in detail: `blocks=78 with_linknext=77 walked=78 repeated=0
broken=0 unreached=0 chain_ok=1`.

`--ast-info` builds no window and writes no frame; it parses and prints one
`key=value` per line.  The dialogue preview is **masked** by default
(`speaker=... chars=14 hash=a19d46a4`), because the corpus is an adult title and a
log is not the place for its words; `--show-text` prints them for all-ages data.

**A real frame.**  `d09_hy01_3.ast`, starting at `block_00061` (the one block
whose `bg` this mini-archive actually ships, `image/bg/pink.png`):

```
canvas 1600x900   (the size the backgrounds themselves are: pink.png is 1600x900)
page 1  text=5276 px (framebuffer diff)  ink=5305  glyphs=14  colours 476/1
layer writes=1    refused by policy=8    asset failures=2
frames: /tmp/astpink/page001..003.png
```

The renderer also cannot read what it must not: `AstImageAllowed()` refuses
`image/ev/`, `image/cg/`, `image/fg/` and `image/evmask/` in code, so a `bg`
command that points at event or character art is *counted and located*, never
decoded.  Missing assets are reported with the member name (`image/bg/zbg39a:
asset not found`) instead of leaving a silent black frame.

### 8.2 Sound, with a WAV to prove it

`Vm` now hands its sound tags to a `TagSink` instead of counting them as ignored,
and `AudioBridge` in `tools/agame.cpp` maps them onto `artemis::audio::Engine`.
Nothing about decoding, rate conversion, `.sli` loop windows or mixing is
re-implemented: an archive member is copied to a work directory the first time a
tag names it, and the engine does the rest.

`[bgm file="moon"]` opens the first scene label of the all-ages title
(`*tanabata` in `scenario/scene.iet`).  It is a macro (`*bgm`), so the tag that
reaches the bridge is `[splay
file="bgm/moon" time="" loop="1"]` **with the Lua-ish expression `$'bgm/' + file`
already evaluated** - which is the point: the interpreter expands, the front end
plays.

```
$ agame --script scenario/scene.iet --data-dir <extracted> \
        --audio-capture /tmp/agame-capture.wav --audio-seconds 6
audio tracks started: 1
  bgm   bgm/moon -> .../bgm/moon.ogg  176.99s rms=0.0795 peak=0.366 loop
audio capture: /tmp/agame-capture.wav, 6.00s (264600 frames), rms=0.0807, peak=0.366
$ afinfo /tmp/agame-capture.wav
  Data format: 2 ch, 44100 Hz, Int16, interleaved   estimated duration: 6.000000 sec
```

The capture is **offline**: the decoder thread fills its rings, the capture loop
stands in for the device clock (`BufferedFrames()` before every `Mix()`), and the
result is written as 16-bit PCM.  Nothing sleeps for real audio time and nothing
blocks a render loop, which is what makes a headless run able to prove sound at
all.  Per-track numbers come from decoding each track on its own; the mix's RMS
and peak come from what was actually written.

### 8.3 Effects, asserted on pixels

Section 7.4 is right that `scetween` is not a per-character effect: it is a
*layer property* tween whose `diff` is an increment.  The per-character reveal is
`g.msgspeed = 30`, read out of the game's own `first.iet`.  Both are wired now,
along with `lytween` (absolute, with yoyo), `anime` (a keyframe track, `time` is
how long until the next key arrives - no interpolation), `sfade` (gain 1000 is
the identity) and `quake` (seeded, so a shake is reproducible).

`agame selftest` asserts each one on the all-ages title's real background and real
font, by sampling the *same* effect at two times:

```
PASS fx-msgspeed  g.msgspeed=30; t=0 1 glyph/739 px, t=400 14 glyph/5340 px, end 30 glyph/11092 px at 870 ms
PASS fx-scetween  base 200 diff -100: t=200,150,100 then a second tag -> 0; 714752 px differ between alpha 200 and 0
PASS fx-lytween   top 40->35/1500 yoyo=-1: t=0 40, 1500 35, 3000 40, 4500 35; left 0->400/1000 linear yoyo: 500 200, 1000 400, 1500 200, 2000 0; malformed tag refused=1
PASS fx-anime     top at t=299,599,600,1000 is 40,40,43,45 (40 until 599, 43 at 600 - a jump, no ramp); 6 keys
PASS fx-sfade     gain 1000 identity on 256/256 values; gain 500 -> 0.505 of the luma; ramp at 1000/2000 ms -> 0.505, at 2000 -> 0.000 of it
PASS fx-quake     seeded offsets identical across runs=1, moved=1, first offset (-30,0), 688717 px move, quiet at t=50
```

And on the real first page, `--fx --fx-at N` renders the reveal as it looks N ms
in: 3 glyphs / 1461 px at N=0, 9 / 4081 at 200, 16 / 7503 at 400, 61 / 29216 when
complete.  The baseline without `--fx` is unchanged.

### 8.4 Input wiring and choices, parsed and printed

`setonpush` is a VK code to a script label; the table is now built from every
loaded script and printed with the key names spelled out:

```
setonpush bindings declared: 14 (12 taken during this run)
    key=83   S        -> rclick.iet:save  [macro:396]
    key=76   L        -> rclick.iet:load  [macro:397]
    key=100  NUM4     -> (handler):qsave  [system/develop_tool:5]
    ...
```

`select` is recognised and printed too (`select file= pre= cond=`), but its option
list is not resolved: in the `.ast` corpus the labels it names live in *another*
script, and the target of the one `select` this tree can see (`d02_ik01_1`) is not
in the mini-archive.  `[stop]` still ends a still-frame run - which is what the
harness wants from it; an interactive event loop is not modelled.

### 8.5 What is still not wired

Counted, located, and printed with `first at <block>:<line>`; nothing is silent.

| tag | corpus count | why not |
|---|---|---|
| `extrans` | 34 | a crossfade needs two pictures; the still-frame renderer has one |
| `colortone` | 141 | a colour matrix over the composite; not modelled |
| `fg` `fgact` `fgf` `fgfact` `cgact` `evset` `scene` | 14,523 | character and event art: refused by the image policy, never decoded |
| `movie` | 5 | video playback |
| `excall` | 56 | a cross-file call into another `.ast` |
| `タイトル` | 2 | a CJK command name only a title screen uses |
| `autoplay` `skipstop` `cacheclear` `exfont` `savetitle` | 350 | booked: they change nothing on the stage |
| `scetween` slots | 80 | its `type` is one of the fifteen GUI state slots (`in`/`out`/`show`/`hide`/`on`/`click`/...), and this port has no *layer* for those slots yet, so the 80 `mode="add"` tags that reached the front end were counted as unmatched; the tween itself is wired (see 8.3) |

And the one honest gap in 8.1: a shipped `.ast` title keeps `[chgmsg]`/`[font]` in
a `first.iet` that these archives do not ship (the corpus has 0 font files among
its 1,682 members), so the message box for the `.ast` path is this port's default
and the frame it draws with is a documented fallback from the all-ages title.  The
report says so on every run (`font: ... from fallback: ...`).

### 8.6 Reproducing it

```
cd artemis
cmake --build build --target agame
./build/agame --ast-info ../testdata/artemis-henpri-trial/extracted/script   # 59 files
./build/agame ../testdata/artemis-henpri-trial/extracted/script/d09_hy01_3.ast \
    --start-block block_00061 --frames /tmp/astpink --max-pages 3
./build/agame --script scenario/scene.iet --data-dir ../testdata/artemis-nicolan/extracted \
    --out /tmp/first.png --audio-capture /tmp/agame-capture.wav --audio-seconds 6
SDL_VIDEODRIVER=dummy ./build/agame selftest                                # 18/18
```
