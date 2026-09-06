#!/bin/sh
set -eu

repo="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
if command -v podman >/dev/null 2>&1; then
    engine=podman
elif command -v docker >/dev/null 2>&1; then
    engine=docker
else
    echo 'Rendering tests require Podman or Docker.' >&2
    exit 127
fi

# The compiler image is amd64. Explicit emulation also makes the command usable on Apple Silicon.
image=nickeltypefix-rendering-tests
"$engine" build --platform linux/amd64 --tag "$image" "$repo/test/rendering"
"$engine" run --rm --platform linux/amd64 --network none \
    --volume "$repo:/repo:ro" "$image" sh /repo/test/rendering/run.sh
