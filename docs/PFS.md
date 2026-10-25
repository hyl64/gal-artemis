# The Artemis .pfs archive

Written down from what the format does, so the implementation in `src/pfs.cpp`
can be checked against it rather than against a memory.  Where a claim was once a
guess it now cites the real file that settles it (see `docs/REALDATA.md` for the
whole evidence list).

## Shape

All integers little-endian.  Three layouts exist; the reference is GARbro's
`ArcFormats/Artemis/ArcPFS.cs` (MIT), and real files confirm it:

```
pf2        magic "pf2", count at index offset 4, entries at index offset 8
           (file 0x0F), 12 reserved bytes per entry, names are cp932,
           no encryption and no trailing table
pf4/pf5/   magic "pf<digit>"
pf6/pf8/   offset 0    magic[3]        "pf6", "pf8", ...
pf9        offset 3    u32 index_size  bytes of index, counted from 0x07
           offset 7    u32 index_count number of files
           offset 0x0B entries[]       u32 name_length, name (no terminator),
                                       4 reserved zero bytes, u32 offset, u32 size
                       u32 index_count + 1
                       u64 offsets[]   where each entry's size field sits, from 0x0F
                       u64 0           end marker
                       u32 count_off   where the count above sits, from 0x07
                       ... file contents, in index order
```

`index_size` covers everything from 0x07 to the first byte of content, so it
includes the count, the entries and the trailing table.

Member names use **backslashes** in real archives (`font\GenShin-Mono-Normal.ttf`,
`script\d01_c01_2.ast`); `artemis::NormalizeName` folds them to `/` and lower case
for lookups.

A shipped title can also ship a **bare `root.pfs`** with no `.pfs.NNN` volumes at
all (the WebAssembly release of ニコラン プロトタイプ is one).  `FindArchives`
accepts `<stem>.pfs`, `<stem>.pfs.NNN`, `<stem>.NNN` and a bare `*.pfs`.

## Content encryption

Only versions **4, 5, 8 and 9** carry a key.  The key is

```
key = SHA1(index bytes, i.e. file[0x07 .. 0x07 + index_size))
```

and the content is

```
plain[i] = cipher[i] ^ key[i % 20]
```

**The key starts at zero for each entry, not at the entry's position in the
archive.**  This corrects an earlier version of this document, which claimed
`key[(offset + i) % 20]`.  The real pf8 sample (ラズベリーキューブ WEB体験版,
212 MB, 2891 entries) settles it: the first entry at offset 155003 decodes with
the entry-relative rule to `\x00\x01\x00\x00 ... "FFTM"` - a TrueType font - and
to noise with the archive-offset rule.  GARbro's `ByteStringEncryptedStream`
runs the key from the start of each entry stream, which is the same rule.

`pf6` and `pf2` are **plaintext**: the real pf6 sample (ニコラン プロトタイプ,
44 MB, 125 entries) yields valid TTF, PNG, JPEG and text members with no XOR at
all.  Neither the reference nor the bytes have ever shown a keyed pf6.

Because the format does not state the rule, `src/pfs.cpp` does not assume it: it
tries both rules on a sample of members and keeps the one that sniffs like a real
TTF/PNG/Ogg/script, then prints the decision and its score in `pfstool info`.
On the real 211 MB archive that detection reports

```
key phase   entry  (content score 1200)
```

and the extraction of a member is byte-identical to the reference copy.

## Performance

Listing an archive must not read it.  `artemis::Open` maps the file read-only and
parses only the index; `ExtractTo` decrypts a 256 KiB window at a time, so a
member is written out of a 211 MB archive without the archive or the member being
loaded whole.  `pfstool list` on the 1682-entry, 211 MB volume reads 94,293 bytes
of index and nothing else.

## What is in a game

Real inventories (see `docs/REALDATA.md`) differ by title and by release channel:

- `root.pfs` (WebAssembly release): fonts, `bgimage\*.png|jpg`, `se\*.ogg`,
  `backlog.iet`, `slider.iet`, `system\*.iet` - where `.iet` here is a **KAG
  script**, not an image
- PC volumes: `script\*.ast` (Lua tables), `image\**`, `sound\**`, `system\*.asb`
  (binary bytecode), `*.ipt` descriptors

`.pfs` is the container, and nothing more than the container.

## Tests

`pfstool selftest` writes archives with this implementation's writer in every
convention (pf8 keyed entry-relative, pf8 keyed archive-offset, pf6 plaintext,
pf6 that a build did encrypt), reads them back, compares bytes, checks the key
phase detection picked the right rule, reads a hand-written pf2 fixture, walks a
temp folder of `<stem>.pfs`, `<stem>.pfs.001` and a decoy `.pfs.bak`, streams a
1 MB member out of a mapped archive, and rejects junk.  It needs no game data;
the real archives are separate evidence, recorded in `docs/REALDATA.md`.