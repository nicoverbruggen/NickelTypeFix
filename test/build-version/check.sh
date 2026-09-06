#!/bin/sh
# Run in the build container, where GNU Make and the cross-toolchain are available.
set -eu

cd "$(dirname "$0")/../.."
python3 test/build-version/fingerprint_test.py
work="$(mktemp -d "${TMPDIR:-/tmp}/nickeltypefix-version.XXXXXX")"
trap 'rm -rf "$work"' EXIT HUP INT TERM
fingerprint="$(python3 tools/build_fingerprint.py)"

version() {
    make --no-print-directory SKIPCONFIGURE=print-ntf-version \
        --eval='print-ntf-version:;@printf "%s\n" "$(VERSION)"' print-ntf-version "$@" | tail -n 1
}

test "$(version VERSION=v9.9 NTF_DEV_BUILD=0)" = v9.9
test "$(version VERSION=v9.9 NTF_DEV_BUILD=1)" = "v9.9-dev-$fingerprint"
test "$(version VERSION=v9.9-7-g1234567-dirty NTF_DEV_BUILD=1)" = "v9.9-7-g1234567-dirty-dev-$fingerprint"
test "$(GIT_DIR="$work/no-git" version VERSION= NTF_DEV_BUILD=1)" = "dev-$fingerprint"
echo 'build version tests passed'
