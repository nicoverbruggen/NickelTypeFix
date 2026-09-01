#!/bin/sh
set -eu

cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/nickeltypefix-spacing-test-$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror line_spacing_values_test.cc -o "$out"
"$out"
echo "line-spacing values passed"
