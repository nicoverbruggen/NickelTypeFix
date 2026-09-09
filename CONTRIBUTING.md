# Contributing to NickelTypeFix

Technical guide for building, testing, and changing the mod. These mods follow the shared conventions in [NickelGuidance](https://github.com/nicoverbruggen/NickelGuidance).

## Building

Needs [podman](https://podman.io) (or Docker); the ARM cross-toolchain runs in a container, so your host never needs it:

```sh
git clone --recursive https://github.com/nicoverbruggen/NickelTypeFix   # --recursive: NickelHook is a submodule
cd NickelTypeFix
./build.sh                                                             # make clean all strip koboroot in the NickelBench image
```

This produces `KoboRoot.tgz` at the repo root. `./build.sh <targets>` passes other make targets through; `NICKELTC_IMAGE` overrides the container image. You can also build straight on the host with `make CROSS_COMPILE=/path/to/nickeltc/bin/arm-nickel-linux-gnueabihf- all koboroot`.

Version stamping: NickelHook.mk bakes `git describe --tags --always --dirty` into `NH_VERSION`: the git tag when you're on one, otherwise a commit hash. `build.sh` keeps `.git` out of what it sends the container, so it reads the version on the host and passes it to make. A local build is stamped like a CI one, with `-dirty` when the tree has uncommitted changes. Outside a checkout there is no release version and the logger falls back to `dev`. CI (checkout with `fetch-depth: 0`) produces the authoritative artifacts.

Development builds append `-dev-<fingerprint>` to that version, or use `dev-<fingerprint>` outside a checkout. The 12-digit fingerprint identifies the contents and paths of the build scripts, mod sources and headers, NickelHook sources and headers, and packaged resources. It appears in NickelHook's startup log and every mod log line, so successive uncommitted edits remain distinguishable after the startup log rotates out. It does not identify compiler versions or custom build flags; the startup binary MD5 still identifies the exact library. Computing it requires Python 3. Direct make users must run `make clean` before changing the version, build mode, or compiler flags; `build.sh` does this by default.

Run `test/build-version/check.sh` in the build container to check fingerprint inputs and release, development, and source-archive version stamping. CI runs the same check.

### Development probes

Release builds omit the page and page-boundary probes. Build with `NTF_DEV_BUILD=1 ./build.sh` to compile both probes in. A development build runs them whenever the mod is enabled; there are no probe config keys. The page-boundary probe logs the line boxes before and after Fix 9, its guard refusals, and the resulting page boundaries. The page probe logs a short description of each distinct chapter document. Both observe only and leave pagination unchanged.

## Detour installer tests

Run `test/detour/check.sh` on Linux, including inside the build container. It checks real memory mappings and injects installation failures. CI runs it with the other local checks. Building the same test for ARM also executes the replacement and original Thumb functions; the host run checks their bytes and permissions. Device testing must still check startup and all four detoured functions.

## Rendering regression tests

Run `test/rendering/check.sh` with Podman or Docker. CI uses the same command. It builds the tests against pinned ARM Qt 5.2.1 and runs both shapers under QEMU, checking the dropdown preview, cache replay, line preparation, small-caps rendering, and detour installation. It also checks EPUB delivery callbacks and cancellation. The image fetches checksummed public dependencies on its first build; test execution needs no network or device files. See [coverage and runtime details](test/rendering/README.md).

## Log rotation tests

Run `test/logging/check.sh` with a C compiler. CI runs it in the build container and as an ARM binary in the rendering container. It checks rotation within one session, exact size boundaries, buffered-message order, failed rotation and recovery, concurrent writers, and logging after uninstall. An oversized log left by an older build is preserved in `.old` on its first rotation; subsequent generations stay within the limit.

## Testing on a device

The container uses the toolchain's FreeType backend. Device tests still need to confirm Nickel's hook routing, font reload sequence, iType rendering, and actual reader behavior.

1. Copy `KoboRoot.tgz` into the Kobo's hidden `.kobo` folder over USB.
2. Eject and reboot; the firmware installs it and deletes the tgz.
3. The mod's folder is `KOBOeReader/.adds/nickel-type-fix/` (`doc`, `uninstall`, `config`, and once it logs, `nickel-type-fix.log`). The config is generated from an in-code table rather than a shipped `default` file, so keys added by a later version are appended to an existing config on the next boot.

Boot safety / recovery: NickelHook renames the plugin out of its load path before installing hooks. It starts the three-second restore timer after initialization succeeds. If Nickel exits before the restore, the plugin remains outside its load path on the next boot. A hang during initialization may require a forced restart; failures after the restore are outside this protection. Deleting `.adds/nickel-type-fix/uninstall` (or creating an empty `uninstall-now` file next to it) and rebooting removes the mod and its installed files.

## Logs & debugging

The mod logs to `KOBOeReader/.adds/nickel-type-fix/nickel-type-fix.log` (and to syslog via `nh_log`, viewable with `logread`). Every message carries the mod version. A healthy boot writes the firmware and a compact list showing whether each feature is enabled and active. Active means the feature found every hook or byte patch it needs at startup; it does not mean the feature has already encountered matching book content. Set `ntf_log:1` to include every config value, resolved symbols, and per-fix tracing. Problems always log, and a malformed config turns verbose logging on automatically so mistakes self-diagnose. Before a write would exceed 256 KiB, the log rotates to `nickel-type-fix.log.old`, replacing the previous backup. Rotation runs throughout the session. If it fails, that block is omitted from the file while syslog still receives its messages.

## Firmware compatibility

The mod attaches in more than one place, so there are two compatibility checks:

- **Hooks and lookups.** Each target carries a `//nb <kind> <role|*> <first> <last|*> <symbol>...` annotation. `nickelbench check-source src` verifies every annotation against the recent Kobo 4.x compatibility database bundled in the build image. Hook checks require the expected PLT relocation in the named library. Lookup checks require an exported definition in one of the recorded process libraries. CI runs this after building the mod.
- **Byte patches.** The justification and letter-spacing fixes edit instructions in memory. `test/anchors` (CI job `anchors`) confirms, against real firmware, that every anchor is present and unique and that the expected original bytes sit at each edit offset. A firmware that lacks the pattern is reported as "sits out" and does not fail; an ambiguous match or differing original bytes is a hard failure.

The floor is firmware 4.23.15505. Every hook and lookup is `.optional`, so a missing symbol sits one fix out rather than failing the mod. Targets Kobo 4.x only; 5.x (Qt 6 / Chromium) is out of scope and the mod stays inert there.

## Pull requests

- Add an entry to `CHANGELOG.md` under the heading for the version it will ship in, for any user-visible change (release notes are generated from it).
- Annotate each new or changed hook and lookup with `//nb …`; CI verifies it.
- If you touch a byte patch, update the anchor table so `test/anchors` covers it.
- State the device + firmware you tested on, and attach the relevant `nickel-type-fix.log` excerpt (the PR template asks for both).

## Releases (maintainers)

Check that `CHANGELOG.md` has a complete `## vX.Y` section matching the tag name, tag the commit `vX.Y`, and push the tag. CI builds, extracts that section as the release notes, attaches `KoboRoot.tgz`, and fails if the section is missing or empty.
