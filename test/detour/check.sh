#!/bin/sh
set -eu
ulimit -c 0

cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/nickeltypefix-detour-test-$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror detour_test.cc -o "$out"
"$out"
