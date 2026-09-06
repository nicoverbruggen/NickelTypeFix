# ARM rendering regressions

Run from any directory with Podman or Docker installed:

```sh
/path/to/NickelTypeFix/test/rendering/check.sh
```

CI runs the same command in its `ARM rendering regressions` job. It builds a local container image on the first run and reuses its dependency layers afterward. The repository is mounted read-only. Test binaries and generated files stay in the container's temporary filesystem. Test execution has no network access and needs no device files or NickelHook checkout.

## Coverage

| Path | What fails the job |
| --- | --- |
| ARM detour installer | Incorrect relocated instructions, page permissions, or recovery behavior, including actual calls through the replacement and trampoline. |
| Selected font dropdown | Persistent fallback after font reload, a fallback frame during selection, or a repair that affects the wrong label or outlives its widget. Runs twice with each shaper. |
| Shaping cache | Differences between uncached, first cached, and replayed glyphs, positions, cluster mappings, justification attributes, font sizes, line metrics, or pixels. The test counts calls through the original trampoline to prove that replay occurred. |
| Cache input variations | Incorrect reuse across fonts, sizes, weight, kerning, combining marks, Arabic direction, or the 96-character recording boundary. |
| Small caps | Wrong small-cap glyph IDs, an unexpanded `ffi` ligature, a scaled font engine, changed line metrics, changed ordinary text or a font without `smcp`, or a cache replay that changes the result. |

The Qt tests run once with old HarfBuzz and once with HarfBuzz NG. The shaping test installs the production shaper and font-engine detours on the runtime's actual Qt functions. It includes the production cache and small-caps sources so it can disable recording, count calls through the original trampoline, and check mapping buffers directly, without adding diagnostics to the shipped mod. The small-caps glyph expectation comes from names in the pinned Vollkorn font's `post` table, independently of the mod's GSUB parser.

The existing CI jobs also check page-boundary geometry, line-spacing values, the built interface, symbol compatibility, and firmware instruction anchors.

## Legacy small caps regression

This suite exposed a production bug when small caps ran through old HarfBuzz. Qt had already mapped the uppercase text to glyph IDs. Its legacy shaper reused that buffer for a multi-font engine, so passing lowercase text did not select the lowercase glyphs needed by the mod's `smcp` map. The result retained capital glyph IDs even though the font-engine detour supplied a full-size engine.

The fix remaps the primary font's glyph IDs before shaping. Both shapers must produce the expected small-cap glyphs. A separate check verifies that remapping preserves fallback glyph IDs and keeps the prepared buffer unchanged if it is too small. There are no expected-failure exceptions.

## Runtime and fixtures

The Dockerfile pins the NickelBench image by digest, including its compiler and Qt 5.2.1 build. `dependencies.tsv` pins the emulator, guest C runtime, fonts, and font licenses by immutable Debian snapshot or upstream commit and SHA-256. A missing download, changed checksum, unsupported Qt layout, skipped test, timeout, or failed assertion fails the job. Updating these pins requires rerunning the full suite and reviewing the fixture expectations.

The fixture fonts come from [Noto](https://github.com/notofonts/noto-fonts/tree/ffebf8c1ee449e544955a7e813c54f9b73848eac) and [Vollkorn](https://github.com/FAlthausen/Vollkorn-Typeface/tree/38ab7a896bd6b163ac7f834ec696d6c68e5dedd6). DejaVu Sans comes from the pinned Debian `fonts-dejavu-core` package and provides a font without `smcp`. All font licenses stay beside the fixtures in the image. No font binaries are committed to this repository.

Two test-only adjustments are needed for this old Qt build:

- It ignores `QT_HARFBUZZ`. `qt_runtime.h` checks the pinned QtGui layout and sets its shaper selector before QApplication starts. Those offsets are never used against device firmware.
- Its CPU detection reads `/proc/self/auxv`, which under qemu-user describes the host emulator. `auxv.c` supplies the guest HWCAP value from `getauxval` so Qt sees the emulated ARM CPU's features. The shim is preloaded only into test processes.

The container supplies a pinned Debian ARM C runtime while keeping the toolchain's Qt libraries. The CPU is explicitly `cortex-a9`. The command uses an amd64 container because the compiler image is amd64, including on Apple Silicon.

## Limits

The Qt runtime uses FreeType, not Kobo's iType backend. These tests verify the production repair and shaping code but do not execute Nickel's PLT hook routing, its font reload controller, or the complete kepub reader. Firmware anchor checks verify instruction signatures without executing those patches. The container therefore does not replace device checks for hook integration, WebKit chapter loading, pagination, or display refresh behavior.
