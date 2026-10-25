# Rights and scope

This tree is a from-scratch implementation of the Artemis Engine format for
macOS arm64.  It contains no game data.

`testdata/` (not committed) holds three sets of real data, each obtained from a
source the rights holders publish themselves:

| corpus | what it is | why it is legitimate |
|---|---|---|
| `artemis-nicolan/` | ニコラン プロトタイプ, tales&Vivid | the author publishes the complete work as free, all-ages software |
| `artemis-raspberry-web/` | ラズベリーキューブ WEB体験版, まどそふと / iMel | the publisher's own free web trial (samples only are kept) |
| `artemis-henpri-trial/` | a free append volume from Qruppo | the publisher distributes this add-on at no cost |

No cracked, pirated or paid content is used anywhere.  Provenance, hashes and
HTTP evidence are in `artemis/DATA_SOURCES.md`.

The third corpus is from an adult title.  It is used **only** for the container
and script formats: the unpacked tree keeps scripts, configuration, `.ipt`
descriptors and non-adult assets, and the code refuses to decode
`image/ev`, `image/cg`, `image/fg` and `image/evmask` members from it
(`AstImageAllowed()` for `.ast`, and the IPT tooling never reads those members).

Screenshots under `artemis/screenshots/` and `tests/artifacts/` are local test
evidence.  The artwork, text and fonts in them stay the property of their
authors; nothing here grants any right to redistribute them.
