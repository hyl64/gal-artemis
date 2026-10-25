# The Artemis .ast script

An Artemis scenario is **text, not bytecode** - that much of the original reading
held up.  But the shape was wrong, and the real scripts now in `../testdata/`
show it: a scenario is a **Lua table**, and there are two layouts in the wild.
See `docs/REALDATA.md` for the file-by-file evidence.

## The real format

```lua
-- A: chained blocks (PC releases, e.g. HENPRI)
astver = 2.0
ast = {
	block_00000 = {
		{"savetitle", text="LET'S GO IMPRISONMENT THE PRISON!"},
		{"bg", lv=9, id=9, file="cinema01", time=0, path=":sys/"},
		{"text"},
		text = {
			vo = { {"vo", file="bmbob0003", ch="BmboB"} },
			ja = { { name = {"モブ男B","怖い警官"}, "「お前がやったんだろう！？」", {"rt2"} } },
		},
		linknext = "block_00001",
		line = 20,
	},
	block_00001 = { ... },
}
```

```lua
-- B: a top-level array, dialogue held in a separate lang_NNNNN file
-- (the Web release of ラズベリーキューブ; note there is no astver line)
ast = {
	{ {"savetitle",text="忠犬大将"}, {"bgm", file="bgm001"}, {"text"},
	  crc="e2d16723", lang="lang_00000" },
	{ {"text"}, crc="799cd8ab", lang="lang_00001" },
}
```

What the two share, and what the parser uses:

- A **command** is an array whose first item is a string - the command name -
  with named parameters after it: `{"bg", lv=9, file="cinema01"}`.  A command can
  be bare (`{"cacheclear"}`) or a named field of a block (`delay = { ... }`).
- **Dialogue** lives under `text`, grouped by language: `ja` is the text,
  `vo` is the voice reference.  In the A layout each `ja` entry is
  `{ name = {...}, "line", {"rt2"} }`, where `name` is the speaker (one or more),
  the strings are the line, and the marker objects are tags such as ruby readings
  (`{"ruby", text="べん"}`) or `rt2`.
- Blocks are chained with `linknext` (A layout) or listed in order (B layout);
  B blocks carry `crc` and `lang` instead of dialogue.
- The Lua the real files use: floats (`astver = 2.0`), negative numbers
  (`x=-1000`), `true`, bracket keys (`[600] = {...}`, 258 times across 44 of the
  59 real scripts), and **long-bracket strings** `[[ ... ]]` - required, because
  dialogue contains half-width quotes:
  `[[「先日亡くなった囚人、"ア]], {"ruby", text="べん"}, "便"`.
- Encoding: the real scripts here are UTF-8; `.iet` KAG scripts are Shift-JIS, so
  `asttool text --sjis` exists for those.

`src/lua.{h,cpp}` implements the table dialect once; `src/luaast.{h,cpp}` turns
the table into blocks, commands and lines.  `src/ast.{h,cpp}` still exists and
still parses the **fixture** format below; `asttool` uses it only for files that
do not sniff as `ast-lua`.

## The format this tree first invented (fixture only)

Before any real script was available, this layout was written down from public
notes and used as a fixture:

```
astver = "2.0"
text = {
  "start" = {
    name = "start"
    lines = { "一行目"  [12]  "二行目" }
    volume = 128
  }
}
```

It is **not** what a shipped title holds.  `asttool selftest` keeps roundtripping
it so `src/ast.cpp` does not rot, and `docs/REALDATA.md` records the correction.

## What this implementation provides

```bash
asttool dump      <script.ast>   # real Lua table or fixture: decided from the bytes
asttool text      <script.ast> [--sjis]   # the player-visible lines
asttool real      <script.ast> [--commands] [--limit N]  # blocks, commands, lines
asttool roundtrip <script.ast>   # fixture: parse/dump/parse; real: parse twice
asttool selftest                 # fixtures plus the real samples when present
```

Invariants held to account:

- the fixture text format loses nothing across parse → dump → parse;
- a real script's two parses are identical (there is **no writer** for the real
  layout, by decision - see `docs/REALDATA.md`);
- the real samples parse with their expected block, command and line counts, and
  the **command line itself** is tested, not just the library.

Real results (`asttool selftest`, `dump`):

| script | shape | blocks | commands | lines |
|---|---|---|---|---|
| `d01_c01_2.ast` (HENPRI) | chained, astver 2.0 | 78 | 287 | 77 |
| `d05_g02_3_5.ast` (HENPRI, long strings + ruby) | chained | 310 | 980 | 309 |
| `script_オープニング続き.ast` (Web) | arrayed, external lang | 54 | 129 | 0 (in `lang_*`) |

## Next

The *structure* is read; the *semantics* are not.  Still missing, in order of
importance (`docs/REALDATA.md`):

1. The KAG `.iet` scripts that actually drive a scene - `*label`, `[bgm]`,
   `[lyc]`, `[trans]`, `[n]` - are recognised by the sniffer and not parsed.
2. Command semantics: no expression evaluation, no `[if]` conditions, no macro
   expansion, no Lua host API, no VM.
3. The external `lang_NNNNN` files B-layout scripts point at are not opened.