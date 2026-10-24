# What a real Artemis title looks like on disk

Recorded from public reverse-engineering write-ups of shipped games — the port has no
Artemis data of its own to look at, so this is the next best thing: what the files are
called and how they are arranged.  Sources are the [RE] articles at galgelabo.fc2.page
(for example the one for *アマカノ3*, engine: Artemis).

## The executable and the archives

```
<Game>.exe            the engine binary (Windows)
<Game>.pfs.<NNN>      the game's data, split by kind
```

The `.pfs` archives carry a **numeric suffix** that says what is inside:

| Suffix | Contents |
|---|---|
| `.000` | character voices |
| `.001` | character sprites (立ち絵) |
| `.002` | backgrounds, event CG, sprites |
| `.003` | event CG |

So `Amakano3.pfs.002` is a whole archive of its own, not a part of one.  A reader has
to accept both `<name>.pfs` and `<name>.pfs.NNN`, which this tree's `pfs.cpp` does not
do yet — it takes one file.

## The paths inside

```
root\image\ev\ev_<Size><Group>_<Zoning>_<ID>_<Difference>.<ext>
root\image\fg\<Character>_<ID>.psb
```

- `ev_*` are event CGs, named by size, character group, zoning (`c` ordinary, `h`
  adult), a two-digit id and a two-digit difference.
- `fg/*.psb` are character sprites, and **`.psb` is the Emote PSB format** — a tag-based
  binary much like Flash's, not a plain bitmap.  That matters: sprites are not images a
  decoder can read straight, they are a scene description the runtime composes.
- Earlier write-ups of other Artemis titles use `.iet` for still images; both spellings
  appear across games.

## What this means for the port

1. `pfs` has to learn the `.pfs.NNN` spelling before it can open a shipped archive.
2. The still images are `.iet`; the sprites are `.psb`.  Neither is a format we can
   guess: `.iet` needs a title's own bytes to reverse (nothing public documents it), and
   `.psb` is Emote's, which has independent documentation but is a much larger job than
   a bitmap decoder.
3. Until one of those exists, the Artemis runtime in this tree plays text-only
   scenarios — which is what `agame` does, and what its five self tests cover.
