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

Version stamping: NickelHook.mk bakes `git describe --tags --always --dirty` into `NH_VERSION`: the git tag when you're on one, otherwise a commit hash. `build.sh` keeps `.git` out of what it sends the container, so it reads the version on the host and passes it to make. A local build is stamped like a CI one, with `-dirty` when the tree has uncommitted changes. Outside a checkout there is no version and the logger falls back to `dev`. CI (checkout with `fetch-depth: 0`) produces the authoritative artifacts.

### Development probes

Release builds omit the page and page-boundary probes. Build with `NTF_DEV_BUILD=1 ./build.sh` to compile both probes in. A development build runs them whenever the mod is enabled; there are no probe config keys. The page-boundary probe logs the line boxes before and after Fix 9, its guard refusals, and the resulting page boundaries. The page probe logs a short description of each distinct chapter document. Both observe only and leave pagination unchanged.

## Testing on a device

1. Copy `KoboRoot.tgz` into the Kobo's hidden `.kobo` folder over USB.
2. Eject and reboot; the firmware installs it and deletes the tgz.
3. The mod's folder is `KOBOeReader/.adds/nickel-type-fix/` (`doc`, `uninstall`, `config`, and once it logs, `nickel-type-fix.log`). The config is generated from an in-code table rather than a shipped `default` file, so keys added by a later version are appended to an existing config on the next boot.

Boot safety / recovery: NickelHook's failsafe (`failsafe_delay = 3`) uninstalls the mod if Nickel crashes within ~3 s of boot; power off within that window to recover a bad build. Deleting `.adds/nickel-type-fix/uninstall` (or creating an empty `uninstall-now` file next to it) and rebooting also removes it, along with everything the mod installed.

## Logs & debugging

The mod logs to `KOBOeReader/.adds/nickel-type-fix/nickel-type-fix.log` (and to syslog via `nh_log`, viewable with `logread`). Every message carries the mod version; the startup block logs the mod version, the firmware version, the effective config, and the resolved-symbol map, unconditionally, so an attached log is diagnostic even with logging off. A healthy boot writes nothing else. Set `ntf_log:1` in the config for verbose per-fix tracing (a malformed config turns it on automatically so mistakes self-diagnose). The log is size-capped (256 KB) and rotates once to `nickel-type-fix.log.old`.

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
