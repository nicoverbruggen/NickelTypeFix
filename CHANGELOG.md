# Changelog

## v0.9

### Added

- Long chapters open several times faster. The reader now uses Qt's newer text shaper and remembers text it has already shaped.
- Long chapters open faster again. WebKit used to lay a chapter out twice and throw the first one away; that pass is now skipped.
- The reader can expose 24 line-spacing choices instead of the stock 15, running from 0.80 to 1.50.

### Fixed

- Narrow custom line spacing no longer clips text or pushes paragraphs onto separate pages.
- A malformed font file can no longer make the capital-spacing fix read or write outside the font while inspecting it.

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
