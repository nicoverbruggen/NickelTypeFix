# NickelTypeFix

NickelTypeFix fixes text rendering on Kobo eReaders and reduces the work needed to open long chapters. It is a [NickelHook](https://github.com/pgaskin/NickelHook) mod for the Qt 5 reader in Kobo firmware 4.x.

Each fix has a configuration switch. Missing hooks or incompatible code patterns disable the affected fixes. See [Safety](#safety) for the checks and their limits.

> [!IMPORTANT]
> Supports **Kobo firmware 4.21.15015 or later in the 4.x series**. Firmware 5.x is unsupported for now.

## What it fixes

### Glyphs

| Problem | Fix | Number |
| --- | --- | --: |
| Copies of the same letter sit a pixel higher or lower on the line. | Disables font hinting to keep the baseline even. | **#1** |
| Capitals have extra spacing in ordinary prose because the reader applies the font's `cpsp` feature. | Removes `cpsp` when loading the font, preserving its other features. | **#7** |
| Small caps look thin because the reader makes them by shrinking ordinary capitals. | Uses the reading font's `smcp` glyphs and kerning when available. | **#14** |

Real small caps need `optimizeLegibility`. Fonts without `smcp`, fallback glyphs and right-to-left runs keep their original rendering. The fix supports the common lookup formats, not every OpenType font.

### Spacing

| Problem | Fix | Number |
| --- | --- | --: |
| Justified kepubs stretch letters or leave uneven gaps at sentence boundaries. | Includes the spaces at `koboSpan` boundaries in justification. | **#3** |
| Justification adds unwanted space around dashes, ellipses and curly quotes. | Corrects which characters receive justification spacing. | **#4** |
| CSS `letter-spacing` spreads letters apart but leaves word spaces too narrow. | Applies the same spacing to the spaces. | **#5** |

These fixes need WebKit's complex text path. `optimizeLegibility` enables it; a book's `letter-spacing` can also select it.

### Page layout

| Problem | Fix | Number |
| --- | --- | --: |
| Vertical Japanese and Chinese text has sideways or misplaced glyphs with `optimizeLegibility`. | Restores WebKit's vertical rendering path. | **#2** |
| A line is clipped or split across two pages. | Corrects overlapping line boxes and keeps each matched line on one page. | **#9** |
| The reader's alignment setting moves centred images to the left margin. | Restores the image centring specified by the book. | **#10** |
| An oversized inline initial creates an extra gap below the first line. | Removes the initial's excess line height before pagination. Floated initials keep their layout. | **#11** |

The page-boundary fix leaves layouts alone when it cannot establish safe page boundaries.

### Font selection

| Problem | Fix | Number |
| --- | --- | --: |
| A chapter uses a fallback font or line spacing calculated before the selected font loads. | Reapplies the selected font and its line spacing after the chapter loads. | **#6** |
| A font with a number in its name, such as `Source Serif 4`, falls back to the default font. | Quotes the font family in the reader's CSS. | **#8** |

### Chapter loading

| Problem | Fix | Number |
| --- | --- | --: |
| Long chapters spend much of their load time shaping text. | Enables HarfBuzz NG, caches shaped text and skips repeated line preparation. | **#12** |
| WebKit performs layout during loading, then discards that work. | Skips intermediate layout during a chapter load. | **#13** |
| Nickel waits 100 ms between local EPUB chunks. | Delivers the next chunk asynchronously without the fixed pause. | **#15** |

The shaper switch affects text throughout Nickel and can change glyphs, spacing or line breaks. It includes compatibility repairs for the font dropdown and vertical glyphs. The cache preserves the selected shaper's output. See the [measurements and implementation](ABOUT.md#fix-12--slow-chapter-opening--ntf_fast_shaping) and [ARM rendering tests](test/rendering/README.md) for the checks behind these fixes.

The [technical notes](ABOUT.md) explain each defect and its cause. The [device fixtures and test guide](test/fixtures/README.md) provide public-domain books for comparing the results yourself.

## What is `optimizeLegibility`?

It selects WebKit's complex text path, which supports advanced typography such as ligatures and kerning. It also exposes several rendering defects that this mod addresses. The result depends on the font, book styling, and reader settings.

It's off by default and is a manual opt-in in the Kobo config file (**not** a UI setting). Edit `KOBOeReader/.kobo/Kobo/Kobo eReader.conf` and add:

    [Reading]
    webkitTextRendering=optimizeLegibility

After doing that, reboot. The setting enables the text path used by the justification and small-caps fixes. Hyphenation still depends on the book's language, styling, and reader support. The vertical-text fix returns vertical books to WebKit's simple path.

## Screenshots

These Nickel page captures use the [public-domain test books](test/fixtures/README.md). Roman text uses Libron, with PT Serif for capital spacing and Source Serif 4 for font loading. Japanese uses Kobo Tsukushi Mincho. The guide records the device layout, firmware, settings and config switches for each comparison.

The excerpts come from Lewis Carroll's *Alice's Adventures in Wonderland* and Natsume Sōseki's *I Am a Cat* (吾輩は猫である). Read the [Japanese original at Aozora Bunko](https://www.aozora.gr.jp/cards/000148/files/789_14547.html), or see [Nick Bradley's 2025 English translation, Volume One](https://www.penguin.co.nz/books/i-am-a-cat-9781784879792).

The middle image shows changed ink: red marks its original position, green marks its fixed position, and white is unchanged. Each pair uses the same crop.

### Fix #1: Glyph hinting

This pair shows the change from hinted to unhinted Libron at reading size 42. Repeated glyphs were already aligned in this sample, so it demonstrates the rendering change rather than a reproduced baseline defect.

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/wobble.png" alt="wobble original" width="250"> | <img src="docs/highlight/wobble-diff.png" alt="wobble diff" width="250"> | <img src="docs/screenshots/wobble-free.png" alt="wobble fixed" width="250"> |

### Fix #2: Vertical (tategaki) CJK text

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/cjk-broken.png" alt="vertical original" width="250"> | <img src="docs/highlight/cjk-diff.png" alt="vertical diff" width="250"> | <img src="docs/screenshots/cjk-correct.png" alt="vertical fixed" width="250"> |

### Fix #3: Justified text at koboSpan boundaries

In the original, “rate!” has stretched-out letters but barely any space before “However”. The fix restores normal letter spacing and distributes the extra space between words.

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/justification-broken.png" alt="justify original" width="250"> | <img src="docs/highlight/justify-diff.png" alt="justify diff" width="250"> | <img src="docs/screenshots/justification-correct.png" alt="justify fixed" width="250"> |

### Fix #5: Letter-spacing on spaces

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/letterspacing-broken.png" alt="letter-spacing original" width="250"> | <img src="docs/highlight/letterspacing-diff.png" alt="letter-spacing diff" width="250"> | <img src="docs/screenshots/letterspacing-correct.png" alt="letter-spacing fixed" width="250"> |

### Fix #6: Font loading and line spacing

Nickel can calculate line spacing before the selected font loads. With Source Serif 4, the first opening then has tighter lines than the same page after changing the font size and returning. The fix recalculates the style after loading the font, so spacing is correct on the first opening. These captures keep font-family quoting enabled in both versions.

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/font-loading-broken.png" alt="font loading original" width="250"> | <img src="docs/highlight/font-loading-diff.png" alt="font loading diff" width="250"> | <img src="docs/screenshots/font-loading-correct.png" alt="font loading fixed" width="250"> |

### Fix #7: Capital spacing (cpsp)

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/cap-broken.png" alt="capital spacing original" width="250"> | <img src="docs/highlight/cap-diff.png" alt="capital spacing diff" width="250"> | <img src="docs/screenshots/cap-correct.png" alt="capital spacing fixed" width="250"> |

### Fix #9: Page-boundary clipping

At line spacing 0.80, the original page ends with a strip of the next line. The fixed version leaves that line for the next page. Compare the following page [before](docs/screenshots/pagecut-next-broken.png) and [after](docs/screenshots/pagecut-next-correct.png) to check that the line remains whole.

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/pagecut-broken.png" alt="page-boundary original" width="250"> | <img src="docs/highlight/pagecut-diff.png" alt="page-boundary diff" width="250"> | <img src="docs/screenshots/pagecut-correct.png" alt="page-boundary fixed" width="250"> |

### Fixes #10 and #11: Centred images and drop caps

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/opener-broken.png" alt="openers original" width="250"> | <img src="docs/highlight/opener-diff.png" alt="openers diff" width="250"> | <img src="docs/screenshots/opener-correct.png" alt="openers fixed" width="250"> |

### Fix #14: Real small caps

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/smallcaps-broken.png" alt="small caps original" width="250"> | <img src="docs/highlight/smallcaps-diff.png" alt="small caps diff" width="250"> | <img src="docs/screenshots/smallcaps-correct.png" alt="small caps fixed" width="250"> |

## Configuration

Settings are read from `KOBOeReader/.adds/nickel-type-fix/config` (auto-created with these defaults on the first boot; there's no shipped template file). Changes take effect on reboot.

**Every fix is on by default, unless you turn it off**: a key that isn't in your config uses its default, which is why the config only ever needs to list the things you want to change.

When you update the mod, any keys added by the new version are appended to your existing config on the next boot, with your own settings left untouched, so a new fix arrives enabled and the file stays complete without you editing anything.

| Key | Default | Meaning |
|-----|---------|---------|
| `ntf_enabled` | `1` | Master switch. `0` disables rendering changes; the plugin still loads, reads config, and logs startup. |
| `ntf_no_hinting` | `1` | Fix #1: load glyphs unhinted. |
| `ntf_hinting_allowlist` | *(empty)* | Families to keep natively hinted, comma-separated, e.g. `Georgia, Kobo Nickel`. |
| `ntf_vertfix` | `1` | Fix #2: vertical (tategaki) text. |
| `ntf_justify_kospan` | `1` | Fix #3: justification at koboSpan boundaries, the main one. |
| `ntf_justify_punct` | `1` | Fix #4: justification around punctuation. |
| `ntf_letterspace_spaces` | `1` | Fix #5: give spaces the same letter-spacing as the letters. |
| `ntf_kepub_fontfix` | `1` | Fix #6: re-apply the reading font on each kepub chapter. |
| `ntf_cpsp_fix` | `1` | Fix #7: strip `cpsp` so capitals aren't spaced apart in body text. |
| `ntf_quote_fontfamily` | `1` | Fix #8: quote the injected font family so numbered names apply. |
| `ntf_pagecut_trim` | `1` | Fix #9: keep complete lines on one page when their line boxes overlap a page edge. |
| `ntf_center_images` | `1` | Fix #10: keep a centred image centred when text alignment is set to left. |
| `ntf_dropcap_fix` | `1` | Fix #11: stop an oversized drop cap pushing the line under it down. |
| `ntf_fast_shaping` | `1` | Fix #12: use Qt's newer text shaper, remember shaped text, and skip repeated preparation of ready text lines. |
| `ntf_skip_parse_layout` | `1` | Fix #13: skip the layout WebKit does mid-parse and then discards. |
| `ntf_smallcaps` | `1` | Fix #14: use the reading font's own small caps for `font-variant: small-caps`. |
| `ntf_fast_epub_delivery` | `1` | Fix #15: remove the fixed 100 ms pauses between local EPUB chunks. |
| `ntf_more_spacing` | `0` | Replace Kobo's 15 line-spacing choices with 24 closer ones, from `0.80` to `1.50`. |
| `ntf_log` | `0` | Verbose logging to `nickel-type-fix.log`. Problems are logged either way. |

Detected installation failures, safety trips, and config errors are logged regardless of `ntf_log`. A healthy boot also logs the firmware, build identity, and feature status. Set `ntf_log` to `1` for detailed traces. An active status means the required installation checks passed; it does not confirm correct rendering of every book or font.

## Compatibility

Supported on Kobo **software version 4.21.15015 and later in the 4.x series**.

**Kobo software 5.x is unsupported.** The mod targets the Qt 5 / QtWebKit stack used by 4.x firmware.

Compatibility checks cover firmware targets rather than a list of device models. Passing those checks does not replace testing on the device.

The three in-memory byte patches and the WebKit layout detour locate code by instruction patterns. The anchor checks cover firmware 4.21.15015 through 4.46.23836 and verify that each pattern is unique and carries the expected bytes. The two Qt detours resolve exported function symbols instead. None uses a fixed firmware address. A missing target or failed compatibility check skips the affected fix or keeps its stock behavior. These checks cannot detect every change in reader behavior.

Earlier 4.x releases are untested. The support floor is not a runtime version gate; the mod attempts the same per-fix checks there. On 4.21.15015, the smallest optional line spacing can still leave a thin fragment of the previous line at the top of a page.

## Safety

The mod checks each fix before applying it and keeps a boot failsafe armed during installation. A missing target skips the affected fix. If a failed code write cannot be safely rolled back, the mod stops startup and requests a reboot.

### Whole-mod boot failsafe

Before any hook or in-memory patch is applied, NickelHook renames the plugin to `libnickeltypefix.so.failsafe`. It starts the three-second rename-back timer only after NickelTypeFix has initialized successfully.

If Nickel crashes or hangs during initialization, the rename-back timer has not started. On the next boot the plugin is absent from its load path and does not load. A hung device may still need a forced restart.

Once the timer restores the filename, this protection ends. It does not detect later crashes or rendering errors, and a successful startup does not prove that the mod is safe for every subsequent operation.

### Per-fix graceful degradation

A fix stays off when its required hooks or code checks fail. Features that share a detour also depend on that detour installing successfully.

1. Hooked and looked-up symbols are optional: if a symbol isn't present on a given firmware, that fix does not run (instead of aborting the mod).

2. If a byte-patch fix (justification or letter-spacing) can't locate its instruction pattern, or the bytes at a target site aren't what's expected, that fix logs and is skipped. When one does apply, all of its edits are located and verified up front and are written both-or-nothing (a mid-write failure rolls the already-patched sites back).

3. The hinting fix carries a persistent `disabled-by-safety` marker: if `FT_Load_Glyph` is ever unexpectedly unavailable at runtime, it records the marker and passes glyphs through untouched on this and every later boot, leaving the vertical and justification fixes running.

4. The hinting marker is written atomically and an unreadable marker is treated as unsafe, so a storage or permission error cannot silently re-enable a fix that previously tripped its safety shutdown.

5. The reader-font fix publishes a new `KepubBookReader` only after its real constructor completes, tracks it through its destructor, and only consumes a pending chapter repair on the same reader view. A missing lifetime hook disables that repair rather than calling an unverified object.

6. Fixes #10 and #11 run a small script inside the book's own page, because what they have to decide (did the book itself centre this image, is this letter a drop cap) can't be written as a styling rule. The script only reads the chapter and sets a style on the few elements it recognises. It adds nothing to the book, sends nothing anywhere, and never touches the book's files. It runs on the reader's own view and nowhere else, and an error in it skips that one update instead of reaching Nickel. [ABOUT.md](ABOUT.md#script-in-the-books-frame) describes it in full.

7. The in-memory patches (justification and letter-spacing) validate the complete target range and instruction alignment before writing, keep the containing page executable so another Nickel thread cannot fault in unrelated code on that page, replace each instruction with one atomic store, verify the bytes, restore the original segment permissions, and roll back every site touched if a later step fails. If a rollback itself cannot be verified, NickelTypeFix logs the failure and invokes the firmware's normal reboot command before the failsafe can be disarmed (with the kernel reboot syscall as a fallback), so the next start is stock.

### Function detours

Fixes #12 through #14 use four detours. Each replaces a function's first eight bytes with a jump into the mod and keeps a callable copy of the displaced instructions in a trampoline:

- The active `QTextEngine` shaper passes through the shaping cache, justification repair, and small-caps processing.
- `QTextEngine::shapeLine` skips preparation when every item on the line already has glyphs and none is a tab or inline object.
- `QTextEngine::fontEngine` supplies the full-size font engine for fonts with real small caps.
- `WebCore::FrameView::scheduleRelayout` suppresses intermediate layout during a tracked chapter load.

These intercept direct calls as well as calls through library imports. The Qt detours apply to callers throughout Nickel, including UI text. They are not restricted to the book renderer.

The shared installer checks the mapped code range, permissions, alignment, and instruction boundaries before changing anything. It rejects the PC-relative instructions recognized by its prologue guard. It prepares and verifies a read-only executable trampoline, verifies the entry patch, and restores the target page's original permissions. A failed installation restores and verifies the original bytes and permissions. If recovery cannot be verified, it uses the same reboot path as the byte patches while the boot failsafe remains armed.

Installation runs only during startup. Replacing the eight-byte entry is not atomic, so no other thread may execute the target during that change. The installer does not suspend threads, and its instruction guard is not a general Thumb relocator. Firmware checks and device testing remain necessary. [ABOUT.md](ABOUT.md#function-detour-installation) describes the mechanism and limits.

## Build

You don't need to build this yourself. You can just download [the latest release](https://github.com/nicoverbruggen/NickelTypeFix/releases/latest). But if you want to, here are the instructions.

To build the mod, you need Docker or Podman. The build script uses the [NickelBench](https://github.com/nicoverbruggen/nickelbench) image, which includes NickelTC and the firmware compatibility checker:

```sh
git submodule update --init
./build.sh
```

This generates a `KoboRoot.tgz` file.

## Install

Copy `KoboRoot.tgz` to the Kobo's `.kobo` folder, eject, and reboot. The mod should automatically install itself. After an automatic restart, when your home screen is visible again, the mod should have loaded!

## Uninstall

Delete `KOBOeReader/.adds/nickel-type-fix/uninstall` and reboot; NickelHook removes the mod on the next boot. The in-memory patches disappear when Nickel exits; the firmware library files are not patched on disk.

## Development

This repository was made with the assistance of large language models. Specifically: Anthropic's Opus 4.8, Opus 5 and Fable 5, as well as OpenAI's GPT 5.5 and 5.6 Sol. 

Use `NTF_DEV_BUILD=1 ./build.sh` to include a source fingerprint in the build version. See [CONTRIBUTING.md](CONTRIBUTING.md#building).

These models were incredibly useful when attempting to reverse engineer and diagnose the actual issues. 

All of the mod was carefully reviewed by the author, and was developed and tested on the author's actual Kobo devices prior to release.

## License

MIT.
