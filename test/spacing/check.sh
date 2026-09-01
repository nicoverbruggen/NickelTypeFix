#!/bin/sh
set -eu

cd "$(dirname "$0")"
bin="${TMPDIR:-/tmp}/ntf-line-spacing-values-test"
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror line_spacing_values_test.cc -o "$bin"
"$bin"
rm -f "$bin"
echo "line-spacing values passed"
