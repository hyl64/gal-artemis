# What a real Artemis title looks like on disk

Recorded from public reverse-engineering write-ups of shipped games, and then
**corrected by the real free releases now in `../testdata/`** (see
`docs/REALDATA.md` for the file-by-file evidence).  Where the write-ups and the
bytes disagree, the bytes win and the correction is marked.

## The executable and the archives

```
<Game>.exe            the engine binary (Windows)
<Game>.pfs            a single archive, as Web/WASM releases ship it
<Game>.pfs.<NNN>      the PC release's data, split by kind
```

The `.pfs.NNN` volumes are each an archive of their own, not parts of one.  A
write-up of *アマカノ3* reads the suffix as a content hint:

| Suffix | Contents (one title, per a write-up) |
|---|---|
| `.000` | character voices |
| `.001` | character sprites (立ち絵) |
| `.002` | backgrounds, event CG, sprites |
| `.003` | event CG |

**Correction from real data**: this is per-title folklore, not the format.  A real
free trial volume (`HENPRI.pfs.060`, pf8, 1682 entries) holds backgrounds, event
CG, scripts, audio and IPT descriptors together, and the free Web release of
ニコラン プロトタイプ is one bare `root.pfs` (pf6, 125 entries) containing fonts,
backgrounds, sound and scripts.  So a reader must accept `<stem>.pfs`,
`<stem>.pfs.NNN`, `<stem>.NNN` and a bare `*.pfs`, and must not infer contents
from the suffix.  `src/pfs.cpp`'s `FindArchives` does exactly that.

## The paths inside

Real member names use **backslashes**: `font\GenShin-Mono-Normal.ttf`,
`bgimage\milkeyway.jpg`, `script\d01_c01_2.ast`, `system\save.asb`.

```
root\image\ev\ev_<Size><Group>_<Zoning>_<ID>_<Difference>.<ext>
root\image\fg\<Character>_<ID>.psb
```

- `ev_*` are event CG, named by size, character group, zoning (`c` ordinary, `h`
  adult), a two-digit id and a two-digit difference.
- `fg/*.psb` would be character sprites in the Emote PSB format - a tag-based
  binary scene description, not a bitmap.  **No `.psb` sample exists in the three
  real corpora here**: the titles surveyed ship standing art as `fg` commands over
  PNG files instead.  So PSB remains a documented format with no local evidence.

### The extension lies - the important correction

Earlier versions of this file said "`.iet` is an image, `.psb` is the sprite".
The real corpus shows that extensions are reused across titles and releases:

| extension | what it actually is |
|---|---|
| `.iet` | **KAG tag script** in ニコラン (Shift-JIS text: `*label`, `[bgm file="moon"]`, `[lyc]`, `[trans]`, dialogue lines) - and an image descriptor in other titles |
| `.ipt` | four different data dialects, only one of which is a composite image |
| `.ast` | a Lua table script (not the invented text format this tree first assumed, and not bytecode) |
| `.asb` | binary script bytecode (`"ASB\0\0"` + items) |
| `.lua` | a real Lua system script |
| `.sli` | text: sound loop-point metadata |
| `.ini` | INI configuration (`[WINDOWS] WIDTH=1024 HEIGHT=698 FPS=60`) |

Consequences for this tree:

1. **Type a file by its contents, never by its extension.**  `src/sniff.h`
   exists for that, and `pfstool list --types` shows what it decides.
2. `docs/REALDATA.md` is the authority on what each layer actually reads.
3. `.psb` cannot be reverse-engineered from the data available here, so it is
   listed as not done rather than guessed at.

## What this means for the port

1. `pfs` reads `<stem>.pfs`, `<stem>.pfs.NNN`, `<stem>.NNN` and bare `*.pfs`,
   lists with only the index in memory, and extracts by streaming (verified on a
   211 MB pf8 volume and a 44 MB pf6 archive).
2. `.ast` real scripts are read (`asttool real`) in both shipped layouts;
   `.asb` bytecode is read (`asbtool`); `.ipt` is parsed in all four dialects.
3. The scenarios that drive an actual screen are `.iet` KAG scripts, and that
   layer is **not implemented** - `agame` still plays this tree's own fixture.
   Nothing here claims to play a real game; see the "Not done" list in
   `docs/REALDATA.md`.