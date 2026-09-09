# Changelog

## v0.11

### Added

- Long chapters load without Nickel's fixed 100 ms pauses between local EPUB chunks. Set `ntf_fast_epub_delivery:0` to keep those pauses.

### Improved

- Reduced chapter layout work by skipping Qt's repeated preparation of text lines whose glyphs are already ready.
- Reduced page-boundary correction work in long chapters by searching only the line rectangles that can affect each page.

## v0.10

### Fixed

- Fixed inconsistent one-pixel letter spacing introduced by fast text shaping, by restoring the old shaper's rounding of kerning adjustments.
- Fixed shaping-cache entries being reused between design-metric and device-metric modes.

## v0.9

### Documentation

- Clarified shared fix dependencies, process-wide text shaping, rendering test limits, and the boot failsafe's recovery window.

### Added

- Development builds include a source fingerprint in their version and log lines, distinguishing them from release builds and earlier uncommitted edits.
- Long chapters open several times faster. The reader now uses Qt's newer text shaper and remembers text it has already shaped.
- Long chapters open faster again. WebKit used to lay a chapter out twice and throw the first one away; that pass is now skipped.
- Opt-in: set `ntf_more_spacing:1` to use 24 line-spacing choices instead of the stock 15, running from 0.80 to 1.50. Disabled by default.
- Small caps in a book now use the reading font's own small cap glyphs when it has them, with the font's kerning, instead of capitals shrunk to 70%. Needs `optimizeLegibility`; fonts without small caps are unchanged.

### Fixed

- The log rotates during a running session before a write would exceed 256 KiB, instead of growing until the next restart.
- Narrow custom line spacing no longer clips text or pushes paragraphs onto separate pages.
- Fixed an integer overflow in the capital-spacing fix's font-table bounds check.
- The reader-font quoting fix now skips `font-family:` text that sits outside a CSS declaration instead of quoting across rule boundaries.

## v0.8

### Added

- Fonts with numbers in their names now apply correctly.
- Page boundaries no longer clip lines of text in kepub books.
- Centred images stay centred when the reader's text alignment changes.
- Oversized drop caps no longer increase the spacing between the first two lines of a paragraph.
- Development builds include page-layout diagnostics for bug reports.

### Fixed

- The letter-spacing fix now applies when both justification fixes are disabled.
- The reader-font fallback fix works again.
- A failed startup now removes every installed hook.

### Improved

- The supported firmware range now starts at 4.23.15505.
- Updates add new settings to existing config files automatically.
- The log rotates after it reaches 256 KB.
- The startup log lists which fixes are enabled and active.

## v0.7

### Added

- Letter spacing now applies to spaces between words.
- Capital spacing no longer affects ordinary body text.

## v0.6

### Fixed

- Justification patches now support halfword-aligned Thumb-2 instructions.

### Improved

- Fix failures fall back to stock behavior instead of crashing Nickel.
- Logging and config initialization are thread-safe.
- The default config is written atomically.
- The reader-font fix can locate firmware-dependent reader data at runtime.
- Justification fixes recognize sites patched by older standalone mods.

## v0.5

### Improved

- Reader and font state is tracked safely across object lifetimes.
- Memory patches validate their targets, apply atomically, and recover safely from failures.
- The glyph-hinting safety marker is written atomically and fails closed.
- Superseded-mod removal no longer follows symbolic links.
- Maintainer documentation covers hooks, firmware assumptions, and safety decisions.

## v0.4

### Added

- Chapters recover when they initially load with the fallback font.
- Config mistakes are reported in the log and enable verbose logging for that boot.

### Fixed

- Vertical text no longer overrides the reading font, stops after chapter changes, or shrinks enlarged dictionary text.
- Healthy boots no longer produce verbose log output.

## v0.3

### Added

- Renamed NickelHintFix to NickelTypeFix.
- Added support for vertical CJK text with `optimizeLegibility`.
- Added justification fixes for sentence boundaries and punctuation.
- Each fix can attach or sit out independently.
- First boot removes the older NickelHintFix and NickelJustifyFix mods.
- Added before-and-after screenshots and safety documentation.

## v0.2

### Added

- Releases are built automatically and include `KoboRoot.tgz`.

### Improved

- Updated the documentation.

## v0.1

### Added

- Initial NickelHintFix release with glyph-wobble correction and a font allowlist.
