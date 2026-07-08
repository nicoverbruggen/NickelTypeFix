# Changelog

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
