# The Artemis .ast script

An Artemis scenario is **text**, not bytecode, which is the single most useful
fact about this engine. `root/script/*.ast` holds a tree of assignments:

```
astver = "2.0"
text = {
  "start" = {
    name = "start"
    lines = {
      "俺たちの新しい日常が、ここから始まる。"
      [12]
      "二行目"
    }
    volume = 128
  }
}
```

## Grammar

```
file    := (name "=" value)*
value   := "{" block "}"
         | string | integer | float | identifier | tag
block   := (member (","? member)*)?
member  := (name | string) "=" value | value
name    := identifier
```

- Values are quoted strings, integers, floats, bare identifiers, `{ ... }`
  blocks and `[123]` tags.
- A block whose entries all carry keys is a **dictionary**; one whose entries do
  not is a **list**. The two spellings appear in the same file, which is why the
  parser decides per entry rather than per block.
- `[123]` is a **tag**: a position in the text the runtime acts on (a pause, a
  control marker). The number is optional.
- Commas between entries are accepted and ignored.
- Encodings: Shift-JIS in the older titles, UTF-8 in the newer ones; the reader
  takes whatever the file holds and the runtime decides.

## What this implementation provides

`src/ast.cpp` parses a script into a tree and writes the tree back out.
`tools/asttool.cpp` exposes it:

```bash
asttool dump      <script.ast>   # parse and print the tree back
asttool roundtrip <script.ast>   # parse, dump, parse again, compare
asttool selftest                 # the same, on scripts this tool writes
```

The property held to account is that **parse → dump → parse loses nothing**: the
canonical form of the two trees must be equal. The self test checks it on a
realistic scenario, on the same scenario with CRLF line endings, and on a
one-line script. It needs no game data.

## Next

The tree is the runtime's input, not its output: what still has to be understood
is what the *keys* mean - which of them name scenes, which lines are shown, how
the tags drive the text - and that needs a real game's scripts to compare
against.
