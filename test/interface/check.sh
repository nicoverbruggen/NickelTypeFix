#!/bin/sh
# Compare the built library's interface against the committed snapshot.
#
# The point is refactoring: moving code between files must not change which functions the
# mod exports, which symbols it hooks, which libraries it looks them up in, what the config
# keys and their shipped defaults are, or the text it injects into a book. If none of those
# move, the rebuild is wired the same way the old one was.
#
# usage: check.sh [path to libnickeltypefix.so]     (default: src/libnickeltypefix.so)
set -eu

cd "$(dirname "$0")"
SO="${1:-../../src/libnickeltypefix.so}"

if [ ! -f "$SO" ]; then
  echo "error: $SO not found — run 'make all' or ./build.sh first" >&2
  exit 2
fi

python3 extract.py "$SO" > current.txt

if diff -u golden.txt current.txt > drift.diff; then
  rm -f current.txt drift.diff
  echo "interface unchanged"
  exit 0
fi

echo "interface CHANGED:" >&2
cat drift.diff >&2
echo >&2
echo "If the change is intended, re-record it:  python3 extract.py $SO > golden.txt" >&2
exit 1
