# NickelTypeFix

This is a [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders that fixes several **text-rendering defects** in the reader's old Qt 5.2 / QtWebKit / Monotype iType stack, and makes books open faster.

Each fix has its own configuration switch and checks its required hooks or code patterns before applying. Some fixes share hooks or detours, so a failed check can disable more than one feature. These checks reduce risk; they do not prove that every font, book, or firmware behaves correctly. See [Safety](#safety) for the checks and their limits.

> [!IMPORTANT]
> This mod supports Kobo software version **4.x only**. See [Compatibility](#compatibility) for details.

## What it fixes

### How glyphs are drawn

| The problem | What the mod does | Fix |
| --- | --- | --: |
| Letters drift a pixel up or down, so the baseline looks uneven. Most fonts are affected. | Loads the font unhinted, which bypasses the problem. | **#1** |
| Some fonts have `cpsp` metadata baked in, a feature meant only for all-caps runs. The reader applies it to body text too. This makes fonts look spaced incorrectly. | Removes `cpsp` from fonts loaded through Qt's application-font API when the file and table checks pass. Other features are retained. | **#7** |
| A book that asks for small caps (`font-variant: small-caps`, which Standard Ebooks uses for names and chapter openings) gets ordinary capitals shrunk to 70%. They come out thin and cramped next to the text, whatever the font carries. | When the reading font has its own small caps (an OpenType `smcp` feature), uses those glyphs at their real size, with the font's own kerning. A font without small caps is unchanged. Needs `optimizeLegibility`. | **#14** |

Small-caps substitution uses the primary reading font. It skips right-to-left runs and glyphs supplied by a fallback font; it does not implement every OpenType lookup format.

### How text is spaced on a line

These fixes act on WebKit's complex text path. `optimizeLegibility` enables that path; a book's `letter-spacing` can also select it without that setting.

| The problem | What the mod does | Fix |
| --- | --- | --: |
| Justified kepubs break at sentence boundaries, leaving uneven gaps. The main justification fix. | Corrects Qt's justifier so the boundary space gets its share. | **#3** |
| Justification skews around punctuation: em and en dashes, ellipses, curly quotes. | Justification is fixed by fixing how text is laid out. | **#4** |
| `letter-spacing` widens the letters but leaves the spaces at their natural width, so tracked text (a heading, a styled caption, spaced small-caps) runs its words together. | Patches Qt's text shaper so the spaces get the same tracking, the way browsers render it. | **#5** |

### How a page is laid out

| The problem | What the mod does | Fix |
| --- | --- | --: |
| Vertical (tategaki) CJK text renders sideways or misplaced under `optimizeLegibility`. | Keeps vertical books on WebKit's correct rendering path. | **#2** |
| Sometimes lines of text seem to be cut off and spread across two page turns. | Repairs line-box overlap at page edges and paints each matched line on one page. Layouts that fail its geometry checks are left alone. | **#9** |
| Setting the text alignment to left (or justified) also drags a centred image to the left margin. | Puts back the centring the book itself asked for, on those images only. An image already centred on the page is left alone. | **#10** |
| A large drop cap at the start of a chapter pushes the line under it down, so the first two lines of the paragraph sit further apart than the rest. | Stops the drop cap inflating its line, early enough that the reader counts the pages from the corrected layout. A drop cap the book floats is already right and is left alone. | **#11** |

### How fast a book opens

The shaping cache reuses results from the selected shaper. Switching to HarfBuzz NG can change glyph selection, spacing, or line breaks because it applies font features differently. The switch affects Qt text throughout Nickel, including its UI, and includes a repair for the selected-font dropdown preview.

| The problem | What the mod does | Fix |
| --- | --- | --: |
| A long chapter can take several seconds to open, and the wait grows with the length of the chapter. | Uses HarfBuzz NG and caches shaped text. The documented device measurements found roughly twice as fast book opening; the gain depends on the book, font, and device. | **#12** |
| A long chapter is laid out twice while it opens, and the first one is thrown away before you ever see it. | Suppresses scheduled layout during a tracked chapter load, while allowing forced layout to finish the chapter. Short chapters may have no intermediate layout to skip. | **#13** |

In the [recorded measurements](ABOUT.md#fix-12--slow-chapter-opening--ntf_fast_shaping), switching shapers changed two line breaks in about 1,940; enabling the cache changed none. The layout-skipping test retained the same 121-page table. These are results for the tested chapter, not guarantees for every book. The [ARM regression suite](test/rendering/README.md) compares cached and uncached rendering under each shaper separately.

### Which font you actually get

| The problem | What the mod does | Fix |
| --- | --- | --: |
| Changing the font (or size) can break under some circumstances, which (incorrectly) reverts to the system font as a result. | Re-applies your reading font on every chapter, so a chapter that drew before the font was ready gets corrected in place. | **#6** |
| Fonts with a number in the name (like `Source Serif 4`) silently fall back to the default font. | Quotes the font name the reader injects, so numbered families work without renaming them. | **#8** |

## Why was this made?

**The built-in reader application for Kobo devices has some rendering issues, especially when you enable `optimizeLegibility`. This mod aims to fix most rendering bugs in the reader application.**

The point is to make `optimizeLegibility` usable by correcting the specific defects listed above. The cause of each defect and the mechanism for its fix is [documented here](ABOUT.md).

Oh, and there's a few other fixes, too!

## What is `optimizeLegibility`?

It selects WebKit's complex text path, which supports advanced typography such as ligatures and kerning. It also exposes several rendering defects that this mod addresses. The result depends on the font, book styling, and reader settings.

It's off by default and is a manual opt-in in the Kobo config file (**not** a UI setting). Edit `KOBOeReader/.kobo/Kobo/Kobo eReader.conf` and add:

    [Reading]
    webkitTextRendering=optimizeLegibility

After doing that, reboot. The setting enables the text path used by the justification and small-caps fixes. Hyphenation still depends on the book's language, styling, and reader support. The vertical-text fix returns vertical books to WebKit's simple path.

## Screenshots

These are actual page captures from my own **Kobo Clara BW** before and after installing the mod.

The middle **diff** overlays the two: **red** is ink the fix removed (its old position), **green** is ink the fix added (its new position), white is unchanged.

(This way, the effect is obvious even where it's subtle on the page.)

### Fix #1: Glyph "wobble" (uneven baseline)

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/wobble.png" alt="wobble original" width="250"> | <img src="docs/highlight/wobble-diff.png" alt="wobble diff" width="250"> | <img src="docs/screenshots/wobble-free.png" alt="wobble fixed" width="250"> |

### Fix #2: Vertical (tategaki) CJK text

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/cjk-broken.png" alt="vertical original" width="250"> | <img src="docs/highlight/cjk-diff.png" alt="vertical diff" width="250"> | <img src="docs/screenshots/cjk-correct.png" alt="vertical fixed" width="250"> |

### Fix #3: Justified text at koboSpan boundaries

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/justification-broken.png" alt="justify original" width="250"> | <img src="docs/highlight/justify-diff.png" alt="justify diff" width="250"> | <img src="docs/screenshots/justification-correct.png" alt="justify fixed" width="250"> |

### Fix #5: Letter-spacing on spaces

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/letterspacing-broken.png" alt="letter-spacing original" width="250"> | <img src="docs/highlight/letterspacing-diff.png" alt="letter-spacing diff" width="250"> | <img src="docs/screenshots/letterspacing-correct.png" alt="letter-spacing fixed" width="250"> |

### Fix #7: Capital spacing (cpsp)

| original | diff | fixed |
|---|---|---|
| <img src="docs/screenshots/cap-broken.png" alt="capital spacing original" width="250"> | <img src="docs/highlight/cap-diff.png" alt="capital spacing diff" width="250"> | <img src="docs/screenshots/cap-correct.png" alt="capital spacing fixed" width="250"> |

### Fix #9: Page-boundary clipping

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
| `ntf_fast_shaping` | `1` | Fix #12: use Qt's newer text shaper and remember text it has already shaped. |
| `ntf_skip_parse_layout` | `1` | Fix #13: skip the layout WebKit does mid-parse and then discards. |
| `ntf_smallcaps` | `1` | Fix #14: use the reading font's own small caps for `font-variant: small-caps`. |
| `ntf_more_spacing` | `0` | Replace Kobo's 15 line-spacing choices with 24 closer ones, from `0.80` to `1.50`. |
| `ntf_log` | `0` | Verbose logging to `nickel-type-fix.log`. Problems are logged either way. |

Detected installation failures, safety trips, and config errors are logged regardless of `ntf_log`. A healthy boot also logs the firmware, build identity, and feature status. Set `ntf_log` to `1` for detailed traces. An active status means the required installation checks passed; it does not confirm correct rendering of every book or font.

## Compatibility

Requires Kobo **software version 4.23.15505+**.

**Kobo software 5.x is unsupported.** The mod targets the Qt 5 / QtWebKit stack used by 4.x firmware.

Compatibility checks cover firmware targets rather than a list of device models. Passing those checks does not replace testing on the device.

The three in-memory byte patches and the WebKit layout detour locate code by instruction patterns. The anchor checks cover firmware 4.23.15505 through 4.46.23836 and verify that each pattern is unique and carries the expected bytes. The two Qt detours resolve exported function symbols instead. None uses a fixed firmware address. A missing or incompatible target skips the affected fix.

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

Fixes #12 through #14 use three detours. Each replaces a function's first eight bytes with a jump into the mod and keeps a callable copy of the displaced instructions in a trampoline:

- The active `QTextEngine` shaper passes through the shaping cache, justification repair, and small-caps processing.
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

Development builds can include always-on layout probes that release builds omit. See [CONTRIBUTING.md](CONTRIBUTING.md#development-probes).

These models were incredibly useful when attempting to reverse engineer and diagnose the actual issues. 

All of the mod was carefully reviewed by the author, and was developed and tested on the author's actual Kobo devices prior to release.

## License

MIT.
