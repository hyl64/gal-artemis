# The Artemis .pfs archive

Written down from what the format does, so the implementation in `src/pfs.cpp`
can be checked against it rather than against a memory.

## Shape

All integers little-endian.

```
offset 0    magic[3]        "pf6" or "pf8" - two generations, same layout
offset 3    u32 index_size  bytes of index, counted from 0x07
offset 7    u32 index_count number of files
offset 0x0B entries[]       u32 name_length, name (no terminator),
                            4 reserved zero bytes, u32 offset, u32 size
            u32 index_count + 1
            u64 offsets[]   where each entry's size field sits, counted from 0x0F
            u64 0           end marker
            u32 count_off   where the count above sits, counted from 0x07
            ... file contents, in index order
```

`index_size` covers everything from 0x07 to the first byte of content, so it
includes the count, the entries and the trailing table.

## Content encryption

The content of every file is XORed with a twenty-byte key:

```
key = SHA1(index bytes, i.e. file[0x07 .. 0x07 + index_size))
for i in 0 .. size: content[i] ^= key[(offset + i) % 20]
```

`offset` is the file's own position in the archive, so the key does not repeat
with the file's own zero-based index. `pf6` archives may leave the content
unencrypted; a reader treats a failed header check as a plain archive rather
than guessing.

## What is in a game

- `root/script/**.ast` - the scenarios, in the engine's own bytecode
- `root/image/**.iet` - images
- `root/sound/**` - audio
- `root/system/**` - the engine's boot scripts and configuration

The next layer of the port is the `.ast` bytecode: its header, its instruction
set and the scenario structures. `docs/AST.md` will hold that once the format is
established; nothing in this tree parses it yet.

## Tests

`pfstool selftest` builds an archive with this implementation's writer, reads it
with its reader and checks that every file comes back byte for byte, that the
content really is encrypted in the archive, and that both a text and a binary
payload survive. It needs no game data.
