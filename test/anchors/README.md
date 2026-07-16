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

    go run .                     # all 4.x firmware from the mirror (downloads to ./corpus, cached)
    go run . -only 4.44.23552    # specific version(s), comma-separated
    go run . -offline            # only what's already cached under ./corpus (no network)

Firmware libs come from pgaskin's public mirror (`kfw.storage.pgaskin.net`, the same source
`kobopatch-testdata` uses). Only each `KoboRoot.tgz` section is fetched, via HTTP range
requests, and the two Qt libs are cached under `corpus/` (gitignored). CI
(`.github/workflows/anchors.yml`) runs this on pushes to `main` with `corpus/` cached, so a
push shows green (or red) before you tag a release.
