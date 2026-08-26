# Interface snapshot

`golden.txt` records what the built library exposes to the host process. `check.sh` rebuilds
that description from `src/libnickeltypefix.so` and diffs it against the snapshot.

This exists for refactoring. Moving a fix into its own file must not change how the mod is
wired, and reading the diff of a 2500-line file will not tell you that with any confidence.
Everything in the snapshot is read out of the compiled library, never out of the source, so
it describes what actually ships.

```sh
make all          # or ./build.sh
test/interface/check.sh
```

## What it covers

| Section | Catches |
|---|---|
| exported hook bodies | a hook body renamed, added or dropped |
| hooked and resolved symbols | a hook pointed at a different function, or one lost |
| target libraries | a symbol looked up in the wrong library |
| config keys and defaults | a key renamed or removed, or a shipped default flipped |
| long string literals | a change to the injected CSS, or to the scripts run in the book's frame |

## What it does not cover

It compares an interface, not behaviour. Logic inside a hook can change without moving any
of the above, so this is a guard against a refactor rewiring something by accident, not a
substitute for testing a fix on a device.

## Re-recording

When a change to the interface is intended, record it in the same commit that makes it:

```sh
python3 test/interface/extract.py src/libnickeltypefix.so > test/interface/golden.txt
```
