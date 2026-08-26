# Changelog

## v0.8

### Added

- **Fonts with a number in the name now apply** (`ntf_quote_fontfamily`, on by default): a reading font whose family name has a word starting with a digit (`Source Serif 4`, `Helvetica 75`, `Bitter 24pt`) silently fell back to the default font. Kobo drops your reading font into the page as an unquoted CSS rule (`font-family: Source Serif 4 !important`), which is invalid CSS because an unquoted family can't start a word with a digit, so WebKit discarded the whole declaration. The mod now quotes the injected family name, so any font applies and there's no need to rename fonts to work around it. It leaves already-quoted rules and generic families alone. Turn it off with `ntf_quote_fontfamily:0`.
- **Page-boundary clipping fix** (`ntf_pagecut_trim`, on by default): in a kepub book a page edge can slice a line of text in half, leaving the missing strip printed at the bottom of the page before it. The reader only does that where one line's box overlaps the next line's box, which is always by a few pixels; this trims each box so it cannot overlap the next, and the reader then moves a line that would straddle the page edge whole onto the next page by itself. It measures nothing about the font, so it works with any font: sideloaded ones, Kobo's built-in ones, and publisher-default books. In offline testing it was never worse than stock in any configuration, left page counts unchanged, and cost under a pixel of extra white space per page. Lines whose letters rise above their own box (rare, tall accented fonts) keep stock behaviour at the box top. It leaves a line alone when the box under it is not plausibly a line at all: a floated drop cap sits beside a line rather than under it and is several times as tall, and a heading trimmed against one would lose its descenders to the next page. Turn it off with `ntf_pagecut_trim:0`.
- **A centred image stays centred when you set the text alignment** (`ntf_center_images`, on by default): with the reader's text alignment set to left, or to justified, a figure the book had centred moved to the left margin. Kobo applies your alignment setting to every block on the page, and an image alone in a block is positioned by that block's alignment, so the book's own centring was overridden along with the text. The mod now reads what the book's own stylesheet says about each of those blocks and puts the centring back where the author asked for it, and only there. An image the author left-aligned, or never styled, is untouched, and nothing changes size, so no text on the page moves. Turn it off with `ntf_center_images:0`.
- **A drop-cap spacing fix, shipped off** (`ntf_dropcap_fix`, off by default): a large initial letter at the start of a chapter inflates its line and pushes the line under it down, so the first two lines of the paragraph sit further apart than the rest (88 px against 69 px on the device it was measured on). Clamping the drop cap fixes the spacing and looks right, but it also makes the paragraph shorter after the reader has worked out where its pages end. Paging backwards into such a chapter can then land you mid-line, with the bottom of a row of letters along the top of the page. It ships off until it can be done without changing the layout that late. `ntf_dropcap_fix:1` turns it on and gets you both the spacing and the paging problem.
- **Three diagnostics for bug reports**, all off by default: `ntf_pagecut_probe` logs the kepub line boxes and where each page boundary landed, `ntf_order_probe` logs the order of chapter load, CSS injection and pagination, and `ntf_page_probe` logs what a page actually contains. They only observe. With any of them on, a page is laid out and cut exactly as it is with them off; only the log gains detail. Leave them at `0` unless a bug report asks for one.

### Fixed

- **The reader-font fallback fix works again** (`ntf_kepub_fontfix`, Fix 6). It has not been engaging since v0.5: a safety check added there was meant to make sure only the reader's own view can trigger the re-apply, but it looked for that view at the wrong place inside the reader object, so the check never passed and the fix silently did nothing — a chapter that first drew in the system (fallback) font stayed that way until you changed a setting or reopened the book. The check now finds the view where it really is (and still refuses to act when it can't prove it), so the re-apply fires again: a chapter that would come up in the wrong font is corrected as it is shown, at the latest on your first page turn in it. If you had this fix enabled on v0.5, v0.6, or v0.7, it was doing nothing; from this version it works as described again.
- **A failed startup now removes every hook it installed.** Rollback looked hooks up in `libnickel` only, so hooks placed in the Qt and WebKit libraries stayed active after the mod had stopped. It now uses the library each hook was installed in. Comes from a NickelHook update.

### Improved

- **New settings are added to your config automatically on update.** The config file is still created once on first boot, but when a later version introduces a key it is now appended to your existing config on the next boot, with your own settings left untouched. An absent key always takes its default, so a new fix arrives in the state it ships in (on, unless the notes above say otherwise) and the file stays complete and self-documenting without any editing on your part.
- **The log file is now size-capped.** Once it passes 256 KB it rotates to `nickel-type-fix.log.old`, so leaving `ntf_log:1` on can't fill up your device.
- **The log now says which build and firmware it ran on.** Every boot writes a short startup block whether or not `ntf_log:1` is set, so a log you attach to a bug report is useful without having to turn anything on and reproduce the problem again.

## v0.7

### Added

- **Letter-spacing now applies to spaces too** (`ntf_letterspace_spaces`, on by default). CSS `letter-spacing` (tracking) widened the letters but left the spaces, and the letter before each space, at their natural width, so any multi-word letter-spaced text ran its words together. They now get the same tracking, so words stay apart. It's an in-memory byte patch to Qt's text shaper (`QTextEngine::shapeText`), in the same family as the justification fixes: nothing is written to any device library, word-spacing is untouched, and it does nothing to text that has no letter-spacing. Turn it off with `ntf_letterspace_spaces:0`.
- **Capital-spacing (`cpsp`) fix** (`ntf_cpsp_fix`, on by default): some fonts carry an OpenType `cpsp` (Capital Spacing) feature meant only for all-caps text, but Kobo's reader applies it to ordinary body text too, so every capital is pushed away from the letter after it and leaves a loose gap (the `D` in `Docks` is the tell). The mod now removes `cpsp` from each font as it loads, for any font, both your sideloaded fonts and Kobo's own, so capitals sit at their normal spacing again. It reads the font as the reader registers it, zeroes just the `cpsp` feature, and hands the edited font back; kerning and every other feature are left untouched, and a font it can't read or that has no `cpsp` loads exactly as before. Turn it off with `ntf_cpsp_fix:0`.

## v0.6

### Fixed

- **Justification patches no longer assume word-aligned instructions** (issue #3): Thumb-2 only guarantees halfword alignment, so on a future firmware build the koboSpan fix could have refused a perfectly valid patch site and sat out. Sites are now accepted at halfword alignment, and a misaligned 4-byte edit is written as two naturally aligned halfword stores (validated against a real firmware image, where the previous check passed only by luck of the build).

### Improved

- **Fixes contain their own failures instead of crashing Nickel:** an out-of-memory error inside the vertical-text or reader-font fix now degrades that one update to stock behavior; a hooked GUI call arriving on an unexpected thread makes the fix sit out with a note in the log; logging and config initialization are thread-safe; and the default config is written atomically, so a power cut during first boot cannot leave a truncated file behind.
- **Future-firmware resilience:** the reader-font fix can now discover the reader's internal layout on firmware where it differs from the validated one (proven from the C++ ABI at runtime; it stays safely inactive if the proof fails), and a justification site already patched by one of the older standalone mods is now recognized as "already patched" instead of being misreported as "could not attach".

## v0.5

### Improved

- **Safety hardening after an implementation audit:** the reader-font fix publishes only fully constructed readers and tracks their lifetime and view identity; the hinting safety marker fails closed and is written atomically; and justification patches validate bounds, alignment, permissions, writes, and rollback results, replace instructions atomically while keeping shared code pages executable, and reboot with the boot failsafe armed if process memory cannot be restored safely.
- **Safer installation cleanup:** superseded-mod removal now uses descriptor-relative operations that do not follow symlinks.
- **Maintainer documentation:** added comments explaining the mod's hooks, firmware assumptions, safety decisions, and future-firmware fallback paths.

## v0.4

### Added

- **Reader-font fix** (`ntf_kepub_fontfix`, on by default): in a kepub book, a chapter could get stuck showing the system (fallback) font instead of your chosen reading font if it drew before the font was ready, and page turns wouldn't fix it. Your reading font is now re-applied on every chapter, in place, without moving your reading position.
- **Config mistakes now explain themselves**: a misspelled setting, a malformed line, or an invalid value in the config file is warned about in the log, and full verbose logging switches on automatically for that boot. If a setting doesn't seem to take effect, the log tells you why.

### Fixed

- **Vertical (tategaki) text fix no longer conflicts with other styling.** Previously it could override your chosen reading font in vertical books, stop working after a chapter change, and make the enlarged dictionary's definition text unreadably small. All three are fixed: the vertical fix now cooperates with the reader's own styling instead of replacing it.
- **The log stays empty on a healthy boot.** Verbose logging (`ntf_log`) is off by default, and nothing is written unless something actually goes wrong (or you turn logging on).

## v0.3

The mod was renamed **NickelHintFix → NickelTypeFix** and grew from one fix to four, each independent and individually configurable via a config file that is created automatically on first boot.

### Added

- **Vertical (tategaki) CJK text fix** (`ntf_vertfix`): vertical Japanese/Chinese books render correctly with `optimizeLegibility` enabled; punctuation, brackets, and long-vowel marks are placed properly.
- **Justification fix for sentence boundaries** (`ntf_justify_kospan`): justified kepubs no longer show a starved gap at sentence boundaries while the rest of the line over-stretches.
- **Justification fix for punctuation** (`ntf_justify_punct`): em/en dashes, ellipses, and curly quotes no longer skew the spacing of justified lines.
- Every fix engages only when it can do so safely on your firmware, and a fix that can't apply sits out without affecting the others. Nothing is ever written to the device's system libraries on disk.
- On first boot, the older standalone mods this one replaces (NickelHintFix, NickelJustifyFix) are removed automatically so they can't conflict.
- Before/after screenshots and a safety writeup in the README.

## v0.2

- Releases are now built automatically, with `KoboRoot.tgz` attached to each release.
- Documentation improvements.

## v0.1

- Initial release as **NickelHintFix**: fixes the glyph "wobble" (letters drifting a pixel above or below the baseline with certain fonts) by rendering glyphs unhinted. A config allowlist lets you exempt font families you want left untouched.
