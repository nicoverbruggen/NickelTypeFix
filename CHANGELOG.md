# Changelog

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
