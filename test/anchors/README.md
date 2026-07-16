# Anchor check

Validates NickelTypeFix's in-memory byte-patch anchors against real Kobo firmware, the
byte-patch analog of a `//libnickel` symbol check.

For each byte-patch fix (the two justification fixes and the letter-spacing fix) it
confirms, in the firmware's own `libQtGui`/`libQtWebKit`, that every anchor is
**present**, **unique**, and has the **expected original bytes** at each edit offset, then
prints the firmware range the fix applies to.

- A firmware whose library doesn't carry the pattern is reported `sits-out` (the mod's
  own fail-safe behaviour) and does **not** fail the check.
- `AMBIGUOUS` (>1 match) or `BYTES-DIFFER` on a firmware that *has* the library is a hard
  failure: the mod would refuse to patch, or patch the wrong bytes.

The anchors and original bytes are parsed straight from `../../src/nickeltypefix.cc`, so
there is a single source of truth; the small per-fix table in `main.go` maps each fix to
its anchor(s), target library, and edit offsets.

## Running

    go run .                     # every tracked firmware at the mod floor (downloads to ./corpus, cached)
    go run . -only 4.44.23552    # specific version(s), comma-separated
    go run . -floor 4.38         # raise the version floor (default 4.21, the mod's minimum)
    go run . -offline            # only what's already cached under ./corpus (no network)

The firmware set is `firmwares.tsv` (version, download URL, zip md5), one entry per version
number at the highest device channel. Libs download straight from Kobo's CDN
(`ereaderfiles.kobo.com`); only each `KoboRoot.tgz` section is streamed and extraction stops
once both Qt libs are out, so a firmware costs ~40MB rather than the full ~160MB zip. The two
libs are cached under `corpus/` (gitignored).

CI (`.github/workflows/anchors.yml`) runs this on pushes to `main` with `corpus/` cached, so a
push shows green (or red) before you tag a release.

## Refreshing the firmware list

`firmwares.tsv` is generated from KoboStuff's `kfw.db.js` (the canonical Kobo firmware
database). To pick up new builds:

    go run mkfwlist.go > firmwares.tsv

`-db` overrides the source. Until [pgaskin/KoboStuff#69](https://github.com/pgaskin/KoboStuff/pull/69)
merges, the newest builds live on its head:

    go run mkfwlist.go -db https://raw.githubusercontent.com/pgaskin/KoboStuff/refs/pull/69/head/kfw.db.js > firmwares.tsv
