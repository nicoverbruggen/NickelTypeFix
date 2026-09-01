#!/bin/sh
set -eu

cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/nickeltypefix-pagecut-test-$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror pagecut_geometry_test.cc -o "$out"
"$out"
