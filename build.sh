#!/usr/bin/env bash
set -euo pipefail

image="${NICKELTC_IMAGE:-ghcr.io/pgaskin/nickeltc:1.0}"
workdir="${PWD}"
scratch="${workdir}/tmp/build"

export COPYFILE_DISABLE=1

if [ "$#" -eq 0 ]; then
    set -- clean all strip koboroot
fi

mkdir -p "${scratch}"

# The source tarball below leaves .git out, so the git describe in NickelHook.mk
# has nothing to read inside the container and every local build would report an
# unknown version. Read it here instead and hand it to make. CI is unaffected: it
# runs make directly against a checkout.
version="$(git describe --tags --always --dirty 2>/dev/null || true)"

tar \
    --no-mac-metadata \
    --no-xattrs \
    --no-acls \
    --no-fflags \
    -C "${workdir}" \
    --exclude=.git \
    --exclude=.DS_Store \
    --exclude=tmp \
    --exclude=KoboRoot.tgz \
    --exclude='*.o' \
    --exclude='*.moc' \
    --exclude=nhplugin.json \
    --exclude=src/libnickeltypefix.so \
    -czf "${scratch}/source.tgz" .

podman run --rm -i \
    --entrypoint sh \
    --env "NH_BUILD_VERSION=${version}" \
    "${image}" \
    -lc '
        set -eu
        mkdir -p /work
        tar -C /work -xzf -
        cd /work
        # NickelHook only recognises the toolchain by the bare prefix, so an
        # absolute CROSS_COMPILE made it warn that this is not NickelTC when it
        # is. The image does not put the toolchain on PATH, so put it there and
        # pass the prefix NickelHook expects.
        PATH="/tc/arm-nickel-linux-gnueabihf/bin:$PATH"
        export PATH
        make "$@" \
            VERSION="${NH_BUILD_VERSION}" \
            CROSS_COMPILE=arm-nickel-linux-gnueabihf- \
            MOC=/tc/arm-nickel-linux-gnueabihf/arm-nickel-linux-gnueabihf/sysroot/usr/bin/moc \
            RCC=/tc/arm-nickel-linux-gnueabihf/arm-nickel-linux-gnueabihf/sysroot/usr/bin/rcc >&2
        if [ -f KoboRoot.tgz ] && [ -f src/libnickeltypefix.so ]; then
            tar -czf - KoboRoot.tgz src/libnickeltypefix.so
        fi
    ' sh "$@" < "${scratch}/source.tgz" > "${scratch}/artifacts.tgz"

if [ -s "${scratch}/artifacts.tgz" ]; then
    tar -xzf "${scratch}/artifacts.tgz" -C "${workdir}"
fi
