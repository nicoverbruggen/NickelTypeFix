# Font dropdown regression

This test runs the production repair against Qt 5.2.1 widgets. It needs that Qt runtime and two font fixtures: `Vollkorn-Regular.ttf` and `NotoSans-Regular.ttf`. Both are available in the firmware's font directory. The test does not enable the mod or install any tracing hooks.

Compile `font_dropdown_test.cc` and `../../src/font_dropdown.cc` with Qt5Widgets and pthreads, using C++11. Generate `font_dropdown_test.moc` from the test source with the matching Qt `moc` and put its directory on the include path. Build outputs belong in a scratch directory.

The Nickel toolchain's Qt ignores `QT_HARFBUZZ`. For that runtime, compile with `-DNTF_TEST_NICKELTC_QT` and link `libdl`. This test-only option sets the selector before QApplication starts. It checks the known toolchain QtGui layout before using private offsets and refuses a different layout. Do not use this option with device firmware libraries.

Run the binary with the two font paths in the order above. Set `QT_QPA_PLATFORM=offscreen` and `QT_QPA_FONTDIR` to an empty directory, so a separately installed font cannot hide the removal. Run once with `QT_HARFBUZZ=ng` and once with `QT_HARFBUZZ=old`. The ARM build can run under QEMU with the toolchain's Qt 5.2.1 runtime.

With NG, removing the selected font must first reproduce a fallback that survives font reload. With the old shaper, it must retain the correct font. The repair must restore the original pixels in both cases. Two cycles check repeated changes. Popup rows, unrelated labels, a different family, an empty family, worker-thread calls and an outdated selection are checked separately. The widgets remain hidden, as the Aa panel can be closed when font loading finishes.

A second phase shows the dropdown and changes its text through the production setter wrapper. It checks that the original setter runs exactly once, then tests the temporary preview image. Setting identical text must not recapture a fallback while the font is absent. Removing the font must leave the intermediate pixels unchanged. Completing the reload must remove the image filter and resume normal font rendering. Two cycles cover repeated selections; further checks cover a changed label text, hiding the label, worker-thread calls, and destruction while a preview is held.
