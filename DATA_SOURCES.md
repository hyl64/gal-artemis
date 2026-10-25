# Artemis Engine real-data sources

What is in `testdata/artemis-*`, where every byte came from, and how a third
party can reproduce it. Everything below was **downloaded and verified on
2026-10-25** from machines that can reach the hosts named here; nothing is
reconstructed from memory.

Status: this tree now has three real Artemis archives, taken from
publisher/author-published free downloads only (no cracks, no paid content,
no DRM circumvention). `artemis/build/pfstool list` and `extract` both work on
all three, and every extracted byte was cross-checked against hashes computed
independently from the archive index: 4698/4698 files at first extraction, and
still 100% of whatever is retained on disk after the scope trims below.

Per-source notes and reproducible commands live in each directory:
`testdata/artemis-nicolan/download.md`, `testdata/artemis-raspberry-web/download.md`,
`testdata/artemis-henpri-trial/download.md`; the per-file manifests are
`testdata/artemis-*/archive_index.txt`.

**What is retained in-tree:** source 1 in full (archive + extracted tree), source
3 as `raw/HENPRI.pfs.060` plus a **non-adult** 318-file extracted subset (its
196 MB zip was removed — re-download recipe in `download.md`), source 2 as
provenance + full 2891-entry manifest + small samples (its 212 MB archive and
extracted tree were removed on request because the URL re-serves them
byte-exactly — `testdata/artemis-raspberry-web/download.md` has the recipe and
the retained hashes). `testdata/artemis-henpri-trial/` **is populated**; it is
not a placeholder.

**Use positioning, one line each:**

1. **ニコラン プロトタイプ** — a complete, author-published free work; primary
   evidence for a plaintext `pf6` single-volume container and for `.ast`/`.iet`
   script syntax in a fully playable context.
2. **ラズベリーキューブ WEB体験版** — an official free trial; evidence for an
   encrypted `pf8` single-volume container, key-phase behaviour, multi-layer
   content (`.ast`, `.iet`, `.lua`, `.asb`, `.ipt`, fonts, audio). Only the
   manifest plus small samples are retained.
3. **ヘンタイ・プリズン 山岸姫瑠ミニアペンド** — a **free official add-on of an R18
   title, used strictly as a format sample**; evidence for the PC
   `<Title>.pfs.<NNN>` multi-volume form. Adult imagery (CG/立絵/ev/evmask) and
   voice/BGV audio were **deleted**; what remains is scripts, configs, `.ipt`
   descriptors, UI art and non-adult sound effects.

---

## 1. Sources at a glance

| # | Work | Author / publisher | Free nature | Archive | Bytes | sha256 |
|---|------|--------------------|-------------|---------|-------|--------|
| 1 | **ニコラン プロトタイプ** (Nicolan Prototype) | 同人サークル tales&Vivid (doujin circle) | Author-published **complete free work**, all-ages, WebAssembly/HTML5 release | `artemis.data` = `root.pfs`, magic **pf6** | 44,161,036 | `8bdfe1e5c57dfa682dc2cf25fca6775c1ff7ca47061ef5e2250ba970dc3a9cb4` |
| 2 | **ラズベリーキューブ WEB体験版** (Raspberry Cube web trial) | まどそふと (Madosoft) / engine+hosting by iMel Inc. | **Official free WEB trial (体験版)** of a commercial title | `artemis.data` = `root.pfs`, magic **pf8** | 222,878,531 | `b6839c3246e3607608e8a1ffe00506a0f3548ea20f4bd67cdbee414035b71c89` |
| 3 | **ヘンタイ・プリズン 山岸姫瑠ミニアペンド** (Hentai Prison – Yamagishi Hiru mini append) | Qruppo | **Official free add-on** data volume (R18 game data, used here only as a format sample) | zip → `HENPRI.pfs.060`, magic **pf8** | zip 205,040,949; volume 211,602,406 | zip `071e333280c2dd3f20137f9e1d8003f565203ce3af89634b7508ccc16308be23`; volume `5aaf5b91bc10d8b5a26446116692b51be84e375923ce8b8e3d9ed51127c7a709` |

**Honest scope statement.** (1) is a whole free game, in its author's web build
form. (2) is an official free trial of a commercial game (the trial is free;
the full game is not, and is not here). (3) is **not a game**: it is one free
appendix data volume plus a one-line placement note — it contains no
executable and cannot be played on its own. It is included because a real
`.pfs.NNN` multi-volume member is exactly what the PC form looks like.

Titles 1 and 2 are the ones that justify the phrase "real game data"; title 3
is a real commercial-format volume sample.

---

## 2. Engine identification evidence

Every claim below is something visible in the downloaded bytes.

* **Archive magic.** `pf6` in `artemis-nicolan/root.pfs`, `pf8` in the other
  two. Both are the Artemis `.pfs` container generations (`pfstool list` prints
  `pf6, 125 file(s)` / `pf8, 2891 file(s)` / `pf8, 1682 file(s)`).
* **Self-identifying config.** `testdata/artemis-raspberry-web/samples/system.ini`
  starts with the literal line `;  Artemis 起動設定ファイル / android用` and
  contains `[WINDOWS] WIDTH = 1280 / HEIGHT = 720` — the engine names itself.
* **Path system.** All index names use backslash separators and the engine's
  own roots: `root\script\*.ast`, `script\*.ast`, `image\*.iet`,
  `sound\*.ogg`, `system\*.lua`, `system.ini`.
* **Script layer.** `.ast` files are *text* Artemis Script:
  * title 2 (older form): first line `ast = {`, tags
    `{"bg",ax=1280,ay=720,…}`, `{"bgm",file="bgm001"}`, `{"fg",ch=…,file=…}`,
    `{"text"}`, with `crc ="…"` / `lang="lang_00000"` per block.
  * title 3 (newer form): first line `astver = 2.0`, then
    `ast = {`, `block_00000 = {`, … i.e. the same format with an explicit
    version header and named blocks.
* **`.iet` is a script layer in these titles, not an image format.** Every one
  of the 24 `.iet` files in titles 1–2 is plain text (checked with `file`):
  title 1's are Shift_JIS/KAG-style scenario and menu scripts
  (`*menu`, `[rp]`, `[var name=… data=…]`, `//` comments), title 2's are UTF-8
  `[tag]` boot/extension scripts (`*top`, `[lua] … [/lua]`, `[calllua …]`).
  **Do not treat `.iet` as images here — classify by content, not extension.**
  The beast-form images in these titles are ordinary `.png`/`.jpg` inside the
  archive, and title 3's image-layer entry files are `.ipt`, which are text
  scripts too (`ipt = {`, e.g. `image/ev/ev_chi_01/zev_chi_01_09.ipt`).
* **Bracket-script layer.** `.iet` files are text in `[tag]` notation
  (`[lyprop …]`, `[calllua …]`, `[var name=… data=…]`, labels `*menu`, `*top`),
  and can embed Lua with `[lua] … [/lua]`.
* **Compiled tables.** `.asb` files are binary and start with the literal magic
  `41 53 42 00` (`"ASB\0"`), followed by a table of tags (`calltag`, `wt`,
  `tag`).
* **Extension census** (computed from the members each `archive_index.txt` lists,
  i.e. the full archive, not only what is retained on disk):

  | tree | files in archive | top extensions | retained on disk |
  |---|---|---|---|
  | artemis-nicolan | 125 | 73 png, 17 ogg, 16 jpg, **16 iet**, 2 ttf, 1 ini | 125 (all) |
  | artemis-raspberry-web | 2891 | 1503 ogg, 747 png, 423 sli, 119 jpg, 53 lua, **21 ast**, **8 iet**, 7 tbl, 4 ipt, 3 asb | 4 text files + 3 `.asb` + 4 `.ipt` + header/index samples |
  | artemis-henpri-trial | 1682 | 756 ogg, 551 png, 228 sli, 66 ipt, **59 ast**, 6 csv, 4 tbl, 4 lua, 3 jpg, 3 bat, 2 glsl | 318 (scripts, configs, `.ipt` descriptors, UI art, SFX) |

  Retained script counts across the tree: **60 `.ast`** (59 henpri + 1 raspberry
  sample) and **17 `.iet`** (16 nicolan + 1 raspberry sample) — both above the
  10-file bar the task asks for, and all of them decrypted/readable.

  Counts of `.psb`: **0** in all three — the PSB/Emote layer does not appear in
  these particular titles (it is Emote, not Artemis); nothing here should be
  read as evidence for `.psb`.

---

## 3. Per-source detail

### 3.1 ニコラン プロトタイプ → `testdata/artemis-nicolan/`

* Landing page (author's own site, WebAssembly build):
  <https://talesvivid.sakura.ne.jp/product/nicolan_p_for_wasm/artemis.html>
  — `HTTP/2 200`, `content-type: text/html`, `content-length: 4831`,
  `last-modified: Sat, 13 Nov 2021 19:37:23 GMT`.
* Data URL: <https://talesvivid.sakura.ne.jp/product/nicolan_p_for_wasm/artemis.data>
  — `HTTP/2 200`, `content-length: 44161036`,
  `last-modified: Mon, 18 Nov 2024 15:33:30 GMT`,
  `etag: "2a1d80c-62731a37fff03"`, `accept-ranges: bytes`.
  Captured 2026-10-25 (CST).
* The loader (`…/artemis.js`, 250,983 bytes) declares
  `REMOTE_PACKAGE_BASE="artemis.data"` and
  `"files":[{"filename":"/root.pfs","start":0,"end":44161036}],"remote_package_size":44161036`
  — the whole file **is** `root.pfs`, there is no outer wrapper.
* Bytes 44,161,036, sha256 `8bdfe1e5c57dfa682dc2cf25fca6775c1ff7ca47061ef5e2250ba970dc3a9cb4`.
* Header: `70 66 36 | 60 16 00 00 | 7d 00 00 00` → `pf6`, `index_size = 0x1660 = 5728`,
  `file_count = 0x7d = 125`. Content stored **plaintext** (PNG/JPEG/OggS/TTF
  headers are intact in the archive; 119/125 entries sniff a known signature).
* Free nature: the circle publishes this work for free on its own site; the
  page carries no separate licence text (all rights reserved by the author).
  Kept locally for engine-compatibility testing only. All-ages.
* Layout: single-volume web build — there is **no** `.pfs.NNN` split here,
  unlike PC releases (see 3.3).

### 3.2 ラズベリーキューブ WEB体験版 → `testdata/artemis-raspberry-web/`

* Landing page: <https://acs.imel.co.jp/wasm/imelmado/raspberrycs_tr/artemis.html>
  — `HTTP/2 200`, `content-type: text/html`.
* Data URL: <https://acs.imel.co.jp/wasm/imelmado/raspberrycs_tr/artemis.data>
  — `HTTP/2 200`, `content-type: application/octet-stream`,
  `content-length: 222878531`,
  `last-modified: Fri, 14 Jun 2019 06:28:42 GMT`,
  `etag: "5d033e9a-d48db43"`, `accept-ranges: bytes`.
  Captured 2026-10-25 (CST). (`…/bin/artemis.data` is 404 — the sibling of the
  html file is the right path.)
* Loader (`…/artemis.js`, 285,165 bytes):
  `REMOTE_PACKAGE_BASE="artemis.data"` and
  `"files":[{"start":0,"audio":0,"end":222878531,"filename":"/root.pfs"}],"remote_package_size":222878531,"package_uuid":"70f671f6-f6a0-4769-8359-986ed6dfb656"`.
* Bytes 222,878,531, sha256 `b6839c3246e3607608e8a1ffe00506a0f3548ea20f4bd67cdbee414035b71c89`.
* Header: `70 66 38 | 74 5d 02 00 | 4b 0b 00 00` → `pf8`,
  `index_size = 0x25D74 = 154,996`, `file_count = 0x0B4B = 2891`.
* Content is XOR-transformed (see §4).
* Free nature: free official web trial (体験版) of a commercial title,
  hosted by iMel for まどそふと. No login, no serial, no DRM step. Kept locally
  for engine testing; copyright stays with the rights holders. All-ages trial.
* Names in this index are **UTF-8** (e.g. `script\みなと-01.ast`,
  `script\オープニング.ast`), not CP932.
* **Retention.** The 212 MB archive and its 2891-file extracted tree are *not*
  kept in-tree (they were verified, then dropped to save space). Kept instead:
  the full 2891-entry manifest `archive_index.txt`, `_pfs_manifest.json`, and
  `samples/` — the first 160 KiB (header + complete index),
  64 stored bytes at offset 155003, plus four small decrypted text files
  (`system.ini`, one `.ast`, one `.iet`, one `.lua`). Every retained file and the
  re-download recipe are in `testdata/artemis-raspberry-web/download.md`. On the
  2026-10-25 full download the extraction verified 2891/2891 files against
  `archive_index.txt` before the tree was removed.

### 3.3 ヘンタイ・プリズン 山岸姫瑠ミニアペンド → `testdata/artemis-henpri-trial/`

* Official product page that links the file:
  <https://qruppo.com/products/henpri/> (Qruppo, publisher's own site).
* Data URL (publisher's sample host):
  <https://sample9.dmm.co.jp/digital/pcgame/qruppo_0004/qruppo_0004mini.zip>
  — `HTTP/1.1 200`, `server: nginx`, `content-type: application/zip`,
  `content-length: 205040949`,
  `last-modified: Thu, 11 Apr 2024 12:14:15 GMT`,
  `etag: "6617d417-c38ad35"`, `cache-control: max-age=2592000`.
  Captured 2026-10-25 (CST).
* Zip (205,040,949 bytes, sha256 `071e333280c2dd3f20137f9e1d8003f565203ce3af89634b7508ccc16308be23`)
  contains exactly three entries:
  * `山岸姫瑠ミニアペンド/` (directory)
  * `山岸姫瑠ミニアペンド/HENPRI.pfs.060` — 211,602,406 bytes,
    sha256 `5aaf5b91bc10d8b5a26446116692b51be84e375923ce8b8e3d9ed51127c7a709`
  * `山岸姫瑠ミニアペンド/適用方法.txt` (68 bytes, CP932):
    `HENPRI.pfs.060をHENPRI.exeがあるゲームフォルダに配置してください。`
* `HENPRI.pfs.060` header: `70 66 38 | 55 70 01 00 | 92 06 00 00` → `pf8`,
  `index_size = 0x17055 = 94,293`, `file_count = 0x692 = 1682`.
* This is the **PC multi-volume form**: members are named
  `<Title>.pfs.<NNN>` and are dropped next to `<Title>.exe`. We hold one member
  (`.060`) plus the instruction file — deliberately, because only that member
  is the free download; the game executable and the other volumes are not here
  and must not be fetched.
* Free nature: the append is distributed free of charge by the publisher's own
  sample host; no payment, no serial, no DRM step. It is R18 game data — it is
  present strictly as format evidence. All identifiers, offsets and content
  hashes are recorded in `archive_index.txt`.
* **This directory holds real data** (the only `.pfs.NNN` multi-volume member in
  this tree, deliberately not an empty placeholder): `raw/HENPRI.pfs.060`
  (211,602,406 B, the volume exactly as shipped in the zip), plus a
  **non-adult 318-file** `extracted/` subset — 59 `.ast`, 66 `.ipt` descriptors
  rescued from `image/ev/`, `system/`, `pc/`, `pa/` UI art, `image/bg/`,
  `image/thumb/`, `sound/se/` — all verified byte-for-byte against
  `archive_index.txt` (318/318 after the trim). Commands and scope:
  `download.md`. The 196 MB zip is not retained (re-downloadable, sha256
  `071e3332…`).
* **R18 scope reduction (done).** Because this source is an R18 title, the adult
  imagery (`image/cg/`, `image/ev/`, `image/fg/`, `image/evmask/`) and the
  voice/BGV audio (`sound/vo/`, `sound/bgv/`) were **deleted**; only format
  evidence (scripts, configs, descriptors, UI art, non-adult SFX) remains. The
  full member list including the deleted members is still recorded in
  `archive_index.txt` (metadata only, no pixels). No other volume and no
  executable of this title may be downloaded.

---

## 4. Archive container facts learned from the real files

Layout (matches `artemis/docs/PFS.md`):

```
0x00  magic[3]           "pf6" | "pf8"
0x03  u32 index_size     bytes of index counted from 0x07
0x07  u32 index_count
0x0B  entries[]          u32 name_len, name, 4 reserved zero, u32 offset, u32 size
      ...
      u32 count+1, u64 offsets[], u64 0, u32 count_off
      file contents, in index order
```

* **Names and paths.** Names are stored without a terminator, are *not*
  NUL-padded, and use `\` as the separator; contents are addressed by absolute
  offsets into the same file. Strings are UTF-8 in title 2, pure ASCII in
  titles 1/3 (CP932 inside the text payloads themselves, e.g. `.iet` in
  title 1 decodes as Shift_JIS).
* **`pf6` content is plaintext** — no transform at all.
* **`pf8` content is XORed with the 20-byte `SHA1(index bytes [0x07 …
  0x07+index_size))`.** The phase matters: on these real archives the keystream
  is **file-relative**, i.e. `plain[i] = stored[i] ^ key[i % 20]`.
  Measured by signature sniffing:

  | archive | `key[i % 20]` (entry-relative) | `key[(offset+i) % 20]` (archive-offset) |
  |---|---|---|
  | raspberry `pf8` | **2371 / 2891** entries sniff a known magic | 111 / 2891 |
  | HENPRI `.pfs.060` `pf8` | **1310 / 1682** | 65 / 1682 |

  (Entries that do not sniff are text scripts, `.sli`/`.ipt`/`.tbl` and fonts
  whose first bytes are not in the sniff set.) `pfstool` picks the same phase on
  its own and reports it: `list` prints `key phase entry` for both `pf8`
  archives and `key phase none (plaintext)` for the `pf6` one.
  Cross-check: `pfstool extract`
  output equals the independently computed plaintext sha256 for files whose
  offset is **not** a multiple of 20 (`image\bg\pink.png` @1,239,248 →
  `6518d4d6…`, `image\bg\zbg50a.png` @1,247,038 → `e505fb39…`,
  `image\hd\anime\line11.png` @28,718,268 → `6d6220b7…`). `docs/PFS.md`'s
  `key[(offset+i) % 20]` wording describes the writer's phase, not what these
  real files use; `src/pfs.h` already exposes both phases and sniffs, which is
  why extraction succeeds.
* **Multi-volume.** `HENPRI.pfs.060` is one member of a numbered set; nothing
  in the member itself says which volume it is or where the rest are — the
  engine resolves that from the executable's directory (`<Title>.pfs`,
  `<Title>.pfs.000`, `.001`, …). A reader therefore needs the companion
  volumes for the full picture; a single `.NNN` still parses and lists fine
  (proved above).

---

## 5. Proof: archive listings and real script content

Full per-file listings (name, offset, size, extension, sha256 of stored bytes,
sha256 after the content transform) are next to each archive:

* `testdata/artemis-nicolan/archive_index.txt` — 125 entries (archive + extracted tree present)
* `testdata/artemis-raspberry-web/archive_index.txt` — 2891 entries (archive removed; manifest + `samples/` retained, see `download.md`)
* `testdata/artemis-henpri-trial/archive_index.txt` — 1682 entries (archive + extracted tree present)

Excerpts (tab-separated, stored bytes then plaintext):

```
# archive: root.pfs
# magic=pf6 index_size=5728 file_count=125 total_bytes=44161036
GenShin-Mono-Normal.ttf	33615507	4978216	.ttf	33228474c8e5…	33228474c8e5…	none(pf6 plaintext)
backlog.iet	5735	17120	.iet	a2e51e74f6ce…	a2e51e74f6ce…	none(pf6 plaintext)
bgimage\futari.png	22855	36937	.png	05ffe30d0227…	05ffe30d0227…	none(pf6 plaintext)

# archive: artemis.data        (raspberry)
# magic=pf8 index_size=154996 file_count=2891 total_bytes=222878531
font\GenJyuuGothic-Monospace-Bold.ttf	155003	11564612	.ttf	5e8582a791bf…	82aaa653ed66…	xor keystream=sha1(index)[i%20]
image\hd\anime\line11.png	28718268	216022	.png	efc940c9f43d…	6d6220b7c8fd…	xor keystream=sha1(index)[i%20]

# archive: HENPRI.pfs.060
# magic=pf8 index_size=94293 file_count=1682 total_bytes=211602406
image\bg\bg50a.png	94300	1144948	.png	17e5581f5fe2…	d4d132a68166…	xor keystream=sha1(index)[i%20]
image\cg\GROUND.png	13153778	1816780	.png	8f0dc70024c3…	ac60f68e34a0…	xor keystream=sha1(index)[i%20]
```

`pfstool` on the real archives:

```
$ artemis/build/pfstool list testdata/artemis-nicolan/root.pfs
testdata/artemis-nicolan/root.pfs: pf6, 125 file(s), key phase none (plaintext)
    33615507     4978216  GenShin-Mono-Normal.ttf
$ artemis/build/pfstool list testdata/artemis-raspberry-web/raw/artemis.data   # run before the archive was dropped
testdata/artemis-raspberry-web/raw/artemis.data: pf8, 2891 file(s), key phase entry
$ artemis/build/pfstool list testdata/artemis-henpri-trial/raw/HENPRI.pfs.060
testdata/artemis-henpri-trial/raw/HENPRI.pfs.060: pf8, 1682 file(s), key phase entry
```

(For the raspberry archive the same `list` is reproducible any time by
re-downloading 222,878,531 bytes — see `testdata/artemis-raspberry-web/download.md`.)

### 5.1 A real `.ast` (raspberry, `script\オープニング続き.ast`, first 20 lines)

```
ast = {
{
{"savetitle",text="忠犬大将"},
{"bg",ax=1280,ay=720,bx=-640,by=-360,file="zbg_13_1a",path=":bg/",time=2000,y=-300},
{"bgm",file="bgm001"},
{"fg",ch="MOB他",file="tmo_noa0001",head="mob",id=20,lv=3,mode=1,mx=0,my=0,path=":fa/mob/"},
{"text"},
crc ="e2d16723",
lang="lang_00000",
},
{
{"text"},
crc ="799cd8ab",
lang="lang_00001",
},
{
{"bg",file="bg_13_1a",path=":bg/",time=500},
{"text"},
crc ="1251f5e6",
lang="lang_00002",
},
{
{"text"},
crc ="799cd8ab",
lang="lang_00003",
},
```

Newer generation (Qruppo, `script\d01_c08_6.ast`, first 10 lines) — note
`astver` and named blocks:

```
astver = 2.0
ast = {
	block_00000 = {
		{"savetitle", text="CLIFFHANGER."},
		{"bg", x=-630, y=0, blur=5, time=50, sync=0, file="zbg24c", path=":bg/", ax=1440, ay=810, bx=-640, by=-360},
		{"se", id=9, loop=1, file="am_naka"},
		{"fg", ch="…", size="z1", time=50, mode=3, resize=1, path=":fg/shu/z1/", file="shu_z1c0110", …},
		{"text"},
		text = {
			vo = {
				{"vo", file="mizushiro0004", ch="MIZUSHIRO"},
```

### 5.2 A real `.iet` (raspberry, `system\first.iet`, first 20 lines)

```
/////////////////////////////////////////////////////
// 起動処理
/////////////////////////////////////////////////////
*top
// ------------------------------------------
//	[debug mode="1" level="2"]
// ------------------------------------------
// 初期化処理
[lua]
function system_initlua(engine)
	e = engine
	flg = {}
	playtime = e:now()				-- プレイ時間
--	e:include("system/dmm.lub")		-- DMM Check
	e:include("system/init.lua")	-- 共通
end
[/lua]
[calllua function="system_initlua"]
[calllua function="system_loadinglua"][wt]
[calllua function="system_dataloading"][wt]
```

Title 1's `.iet` is the scenario itself (CP932), `main.iet` first lines:

```
// ---------------------------------------------------------------------------------------------
//  トップメニュー画面
// ---------------------------------------------------------------------------------------------
*menu
//----------------------------------------------------------------------------------------------

[rp]
[cl_a]//絵全消去、念のため
[var name="g.return" data="1"]
[var name="g.check" data="0"]
[var name="t.nowjump_top" data="0"]
[var name="fontcolor" data="1"]
```

### 5.3 A real `.asb` (raspberry, `system\script.asb`)

```
00000000: 4153 4200 00e2 0000 0001 0000 0007 0000  ASB.............
00000010: 0063 616c 6c74 6167 0000 0000 0002 0000  .calltag........
00000020: 0077 7400 0500 0000 0000 0000 0000 0000  .wt.............
00000030: 0300 0000 7461 6700 0600 0000 0100 0000  ....tag.........
```

### 5.4 Independent verification (second pass, other machine session)

These checks were run against the retained data by the reviewer, not by the
collector, and all of them agree:

* **Local copy == server bytes (title 1).** Re-downloaded `artemis.data` in full:
  200 / 44,161,036 bytes, sha256
  `8bdfe1e5c57dfa682dc2cf25fca6775c1ff7ca47061ef5e2250ba970dc3a9cb4`. Then a
  raw HTTP Range read at offset 6,176,832 returned 16 bytes
  `89504e470d0a1a0a0000000d49484452`, byte-identical to the local copy at the
  same offset.
* **Title 1 structure.** An independent parser reproduces 125 entries; the
  content region 5735 … 44,161,036 has **no holes and no overlaps**; all 73 PNG
  chunk CRCs check out; the decoded PNG geometry is 1024×698, consistent with
  `system.ini`'s `WIDTH`/`HEIGHT`. Every manifest row reads
  `none(pf6 plaintext)`.
* **Title 2 key phase.** From `samples/index_head_160KiB.bin` (which contains
  the complete index, 0x07…155003) the reviewer computed `SHA1(index)` and
  decoded `samples/entry_off155003_64B.bin` to
  `00010000 00140100 00040040 4646544d` — a TrueType sfnt header (`00 01 00 00`,
  20 tables) plus the `FFTM` table tag. The **file-relative** keystream is
  therefore confirmed by a second implementation, and the archive-offset
  variant is falsified for this archive.
* **Title 2 bytecode samples.** Three real `.asb` files and four non-CG `.ipt`
  descriptors were pulled directly from the archive by Range and decrypted; the
  `.asb` heads are `41 53 42 00` + entry count, matching the ASB structure. They
  are retained at `testdata/artemis-raspberry-web/samples/asb/` and
  `…/samples/ipt/` with these sha256:
  `system_save.asb` `8745fb2d6bb7dfb9d0a743d41c57a010aa58bdac455a4db269d3099957ba4c3f`,
  `system_script.asb` `7b0207c94781ade15cde4ea1d28fabcd3df74e4e84524e18446bebb2494c1e36`,
  `system_ui.asb` `9003d73fd6a2344bc93a97209aa90c8a0d06030a9b1cb9858eff92885bac6f6c`,
  `image_hd_anime_line1.ipt` `f2b1e8a8eb294abd3b500a1656428c474eee8911fddf21765348636b65c6d045`,
  `psw_ui_ja_mw_dialog.ipt` `bee56d8d54b260e2c80e4276dc734deeacc06ede617b87e1857f63c8c678bf85`,
  `psw_ui_ja_mw_map.ipt` `4fe3c9d742f9e09153c326789014a0eb1e604c8fdafeb132c5baa366c2e818e4`,
  `psw_ui_ja_save_char.ipt` `ac3e9cba4133c78bfcf63dce5a934f42311893e60ca072af7839e5d79b75df04`.
* **Post-trim integrity (title 3).** After the adult-content deletion, all 318
  remaining files were re-hashed against `archive_index.txt`: 318/318 match
  (the relocated `.ipt` descriptors are matched by unchanged content hash).

Earlier in the review the collector briefly recorded `pf6` content as encrypted;
that was a misreading of a partially assembled local download. It is corrected
here and everywhere else in this document: **`pf6` content is plaintext.**

---

## 6. Directory map

```
testdata/
  artemis-nicolan/
    raw/artemis.data          original download name  (sha256 8bdfe1e5…)
    root.pfs                  same bytes, canonical internal name (hard link to raw/artemis.data)
    extracted/                125 files: main.iet, first.iet, scenario/scene.iet, bgimage/*.png|jpg, bgm/*.ogg, se/*.ogg, fonts, system.ini
    archive_index.txt         125 entries x (offset,size,ext,sha256 stored,sha256 plain)
    download.md               reproducible commands
  artemis-raspberry-web/
    archive_index.txt         2891 entries (full manifest of the removed archive)
    _pfs_manifest.json        same, machine-readable
    samples/index_head_160KiB.bin     header + complete index (0x00…155003)
    samples/entry_off155003_64B.bin   64 encrypted bytes of the first entry (TTF)
    samples/system.ini, script_オープニング続き.ast, system_first.iet, system_ui_title.lua
    samples/asb/*.asb         3 real ASB bytecode tables (decrypted)
    samples/ipt/*.ipt         4 real non-CG .ipt descriptors (decrypted)
    download.md               provenance, hashes, re-download recipe
    (the 222,878,531-byte archive itself is not retained; sha256 b6839c32…)
  artemis-henpri-trial/
    raw/HENPRI.pfs.060        the volume as shipped in the zip, 211,602,406 bytes (sha256 5aaf5b91…)
    extracted/                318 files, non-adult subset: script/*.ast (59), system/**, pc/ja/**,
                              pa/** (UI art), image/bg/**, image/thumb/**, sound/se/**,
                              ipt_descriptors/** (66 text .ipt descriptors rescued from image/ev/)
    archive_index.txt         1682 entries (full manifest, including the removed adult members)
    download.md               provenance + scope (adult content removed)
    (raw/qruppo_0004mini.zip, 205,040,949 bytes, sha256 071e3332…, not retained)
  artemis-tools/
    dl_par.sh                 parallel ranged downloader (numeric part assembly — see §7 warning)
    pfs_min.py                minimal independent .pfs reader (list)
    pfs_extract.py            minimal independent extractor (pf6 plaintext / pf8 XOR)
    mkindex2.py               writes archive_index.txt
    verify_extract.py         re-hashes an extracted tree against an index
```

Note: `file` reports these archives as generic `data`, as expected for a
custom container. `raw/artemis.data` and `root.pfs` in `artemis-nicolan/` are
the same inode (hard link), so the directory listing counts 44 MB once.

---

## 7. Reproducing this

```bash
cd ~/Documents/gal-engines
T=testdata/artemis-tools

# 1) nicolan (pf6, whole free work) — 44,161,036 bytes
mkdir -p testdata/artemis-nicolan/raw
curl -L --retry 3 --max-time 900 -C - \
  -o testdata/artemis-nicolan/raw/artemis.data \
  'https://talesvivid.sakura.ne.jp/product/nicolan_p_for_wasm/artemis.data'
shasum -a 256 testdata/artemis-nicolan/raw/artemis.data
# expect 8bdfe1e5c57dfa682dc2cf25fca6775c1ff7ca47061ef5e2250ba970dc3a9cb4

# 2) raspberry web trial (pf8) — 222,878,531 bytes
mkdir -p testdata/artemis-raspberry-web/raw
curl -L --retry 3 --max-time 900 -C - \
  -o testdata/artemis-raspberry-web/raw/artemis.data \
  'https://acs.imel.co.jp/wasm/imelmado/raspberrycs_tr/artemis.data'
# expect sha256 b6839c3246e3607608e8a1ffe00506a0f3548ea20f4bd67cdbee414035b71c89

# 3) Qruppo append volume (zip + .pfs.060) — 205,040,949 bytes
mkdir -p testdata/artemis-henpri-trial/raw
curl -L --retry 3 --max-time 900 -C - -o /tmp/qr/qruppo_0004mini.zip \
  'https://sample9.dmm.co.jp/digital/pcgame/qruppo_0004/qruppo_0004mini.zip'
# expect sha256 071e333280c2dd3f20137f9e1d8003f565203ce3af89634b7508ccc16308be23
mkdir -p /tmp/qr && python3 -c "import zipfile;zipfile.ZipFile('/tmp/qr/qruppo_0004mini.zip').extractall('/tmp/qr')"
cp '/tmp/qr/山岸姫瑠ミニアペンド/HENPRI.pfs.060' testdata/artemis-henpri-trial/raw/
# the 196 MB zip itself is not retained; the volume above is what this tree keeps

# list / extract with the port
artemis/build/pfstool list  testdata/artemis-nicolan/root.pfs
artemis/build/pfstool list  testdata/artemis-henpri-trial/raw/HENPRI.pfs.060
# raspberry: re-download to /tmp first (its 212 MB archive is not retained), then
#   artemis/build/pfstool list /tmp/ras/artemis.data      -> pf8, 2891 file(s), key phase entry

# or with the independent python tool kept beside the data
python3 $T/pfs_min.py     list    testdata/artemis-nicolan/root.pfs
python3 $T/pfs_extract.py         testdata/artemis-nicolan/root.pfs testdata/artemis-nicolan/extracted
python3 $T/verify_extract.py nicolan testdata/artemis-nicolan/archive_index.txt testdata/artemis-nicolan/extracted
```

For source 3, after extracting to a scratch directory, keep only the non-adult
subset described in `testdata/artemis-henpri-trial/download.md` (scripts,
configs, `.ipt` descriptors, UI art, SFX) — never the CG/立絵/音声 directories.

**Warning about naive ranged downloaders.** Whoever splits a file into
`part.0 … part.15` must reassemble in *numeric* order. `cat part.*` sorts
lexicographically and puts `part.10` before `part.2`; that silently produced a
byte-permuted 44 MB file whose size and magic still looked right. The
assembler in `artemis-tools/dl_par.sh` loops `seq 0 N-1` for exactly this
reason. Always compare the sha256 after downloading.

These hosts are reachable from the machine used here; `github.com` direct
clone, DLsite, Steam, archive.org, MEGA and itch.io are **not** reachable from
it, which is why every route above goes through publisher/author hosts,
`sample*.dmm.co.jp` included.

---

## 8. Routes tried and rejected (2026-10-25, for the record)

| URL / host | Result |
|---|---|
| `https://www.ies-net.com/?cat=21` (Artemis 公式, ふりーむ！category) | 200 — used to discover titles; its 6 listed free games all link to ふりーむ |
| `https://www.freem.ne.jp/win/game/32670` (`結-musubi-` Artemis Version) | 200 but body = "Under maintenance" — ふりーむ is down, unusable |
| `https://talesvivid.sakura.ne.jp/product/nicolan_p.html` | 200; download link points at ふりーむ → dead end; the web build (used) works |
| `https://qruppo.com/download/`, `/trial/` | 404 (no trial page in that shape) |
| `https://qruppo.booth.pm/` | 200; shop sells goods/append, no free trial data |
| `https://lllakolll.web.fc2.com/BADcondDATE/` | timeout (000) |
| `https://www.vndb.org/...` | 503 for crawlers; metadata obtained via `https://api.vndb.org/kana/vn` (200) instead |
| `https://artemis-engine.com/` | timeout (000) |
| `https://www.azarashi-soft.com/`, `https://dlsite`, Steam, archive.org, MEGA, `itch.io`, `madosoft.co.jp`, `keromakura.com` | unreachable/blocked as reported by the previous round; not retried beyond one probe |

Raw probe output for the whole session is kept at `/tmp/scout_log.md`.

---

## 9. Rights and limits

* Data 1 is published free by its author; data 2 is an official free trial of a
  commercial game; data 3 is an officially distributed free add-on volume. All
  three were obtained without payment, login bypass, cracks or DRM
  circumvention.
* Copyright remains with the respective rights holders. The copies here are
  for local engine-compatibility testing; do not redistribute or ship them
  outside this tree.
* No serials, no patched executables, no pirated full titles, no paid content
  leaks were sought or obtained.
* Data 3 is R18-adjacent commercial game data; it is retained only as a binary
  format sample, and no executable is present.

## 10. Second title, fully unpacked (2026-10-25, 19:xx)

The 212 MB volume is now in the tree and unpacked, because one question needed
it: the arrayed `.ast` layout reported **zero** dialogue lines and we assumed the
text lived in a `lang_NNNNN` file.

**That assumption was wrong.**  There is no `lang_NNNNN` file anywhere in the
2891 members, and none was needed.  The text sits beside the block array, inside
the same `.ast`:

```lua
ast = {
	{ {"savetitle", text="…"}, {"bg", …}, {"text"}, crc="18ed3701", lang="lang_00000" },
	…
	text = {
		[1] = { ja = { { "さて、今日の放課後はどうするか。" } } },
		[8] = { name={name="悟"}, ja={ { "「はぎゃあああ…！？」" } } },
		[9] = { name={name="同級生男子Ａ"}, vo={ {"vo", file="com_mobma_0117"} }, ja={ … } },
	},
	label = { … },
}
```

The rule that makes the two tables line up: **a block that carries a `{"text"}`
command consumes the next message number, in block order, starting at 1.**  The
`lang="lang_NNNNN"` field is a *localisation key*, not a filename - with only the
Japanese build present it points at nothing.

With that, `asttool dump` on `script/瑠莉-01.ast` reports 100 blocks / 281
commands / **98 lines**, and `agame --ast-info` over all 21 scripts reports
2,918 blocks / 6,110 commands / **2,880 lines** / 1,231 voice references.

What this title still does not give us: a rendered screen.  Its UI is Lua-driven
(`system/ui.asb` + 53 `.lua` files + `psw/ui/ja/**` localisation), and its
`system/first.iet` boots to `*top` with no text page.  Driving it would mean
implementing the Lua host API, which is a different project from a format port -
so this corpus is recorded as **format coverage only**, not a second playable
title.

Pruned on extraction, as before: `image/hd/{ev,cg,fg,fa,mask,cutin}`,
`sound/vo`, `sound/bgv`.  Kept: `script/`, `system/`, `psw/`, `image/hd/{bg,sd,rule,anime}`,
`sound/{bgm,se,sysse}`, `font/`.  The tree is 111 MB; the volume itself stays in
`raw/`.
