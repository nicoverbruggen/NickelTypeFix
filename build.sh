#!/usr/bin/env bash
set -euo pipefail

image="${NICKELTC_IMAGE:-ghcr.io/nicoverbruggen/nickelbench:recent}"
workdir="${PWD}"
scratch="${workdir}/tmp/build"
dev_build="${NTF_DEV_BUILD:-0}"

if [ "${dev_build}" != 0 ] && [ "${dev_build}" != 1 ]; then
    echo "NTF_DEV_BUILD must be 0 or 1" >&2
    exit 2
fi


if command -v podman >/dev/null 2>&1; then
    container_engine="podman"
elif command -v docker >/dev/null 2>&1; then
    container_engine="docker"
else
    echo "building NickelTypeFix requires Podman or Docker" >&2
    exit 127
fi

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

"${container_engine}" run --rm -i \
    --entrypoint sh \
    --env "NH_BUILD_VERSION=${version}" \
    --env "NTF_DEV_BUILD=${dev_build}" \
    "${image}" \
    -lc '
        set -eu
        mkdir -p /work
        tar -C /work -xzf -
        cd /work
        # NickelHook only recognises the toolchain by the bare prefix. Keep its
        # directory on PATH inside the login shell and pass the expected prefix.
        PATH="/tc/arm-nickel-linux-gnueabihf/bin:$PATH"
        export PATH
        make "$@" \
            VERSION="${NH_BUILD_VERSION}" \
            NTF_DEV_BUILD="${NTF_DEV_BUILD}" \
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
