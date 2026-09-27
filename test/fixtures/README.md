# Device fixtures

These books exercise NickelTypeFix on Kobo firmware 4.x. Copy the files in [books](books) to the device's USB storage, eject it and wait for the import. Keep the `.kepub.epub` extension: an ordinary `.epub` uses a different reader.

The books contain public-domain excerpts, with the same text arranged differently to expose each defect. They have no embedded fonts. The headings and ornament are part of the fixture, not facsimiles of the source editions. English headings are bold; the Japanese title uses normal weight.

## Before comparing

Use firmware 4.45.23697 as the reference for release testing. Record the device model, firmware, mod build, font family and version, font size, line spacing, margins and alignment. Keep those settings equal between captures. Different fonts or page sizes can move a defect to a different page.

1. Enable `webkitTextRendering=optimizeLegibility` under `[Reading]` in `.kobo/Kobo/Kobo eReader.conf`, then reboot. Most spacing cases need this setting.
2. Install the mod. For the baseline, set `ntf_enabled:0` in `.adds/nickel-type-fix/config` and reboot. For the fixed result, set `ntf_enabled:1` and reboot. A device without the mod is also a valid baseline.
3. Open the same chapter through the table of contents, then apply the same reading settings. Do not rely on a saved page number after changing fonts or configuration.
4. Close the reading controls before capturing. Compare the actual letters and spacing as well as page counts. A log reporting an active fix proves that it attached, not that the book exercised it.

For an individual fix, leave the master switch on and change only the key named below. Reboot after every config change. For capital spacing, also disable fast shaping in both runs: NG already omits `cpsp`, so leaving it enabled masks that defect.

Use Libron 0.25 for Roman text and Kobo Tsukushi Mincho for Japanese. Copy sideloaded fonts into `fonts` on the device. [Vollkorn Regular](https://github.com/FAlthausen/Vollkorn-Typeface/blob/38ab7a896bd6b163ac7f834ec696d6c68e5dedd6/fonts/ttf/Vollkorn-Regular.ttf) provides another small-caps test; the [rendering dependencies](../rendering/dependencies.tsv) pin its file and OFL license. A font with a different version or feature set is a separate test case.

## Cases

| Book | Setup and steps | Broken result | Expected fixed result |
| --- | --- | --- | --- |
| `01-baseline` | Use an uninstructed TrueType font. Compare repeated letters in “Reading sample”; try the sizes in “Size sweep”. Toggle `ntf_no_hinting`. | Repeated copies of a glyph have different vertical bounds or baseline positions. | Repeated glyphs align consistently. If the stock result is already even, record “not reproduced”, not a pass. |
| `02-vertical` | Select Kobo Tsukushi Mincho. Read “冒頭” for long-vowel marks and “画はどうかね” for brackets. Change the font size, open an English book, then reopen this book. Toggle `ntf_vertfix`; also compare fast shaping on and off with the vertical fix enabled. | Brackets and `ー` remain horizontal, or punctuation sits in the wrong position. | Brackets enclose vertical text; `ー` follows the column; punctuation uses vertical placement. Fast shaping must preserve these forms across chapter and book changes. |
| `03-justification` | Select full justification in the reader. At reading size 42, inspect “rate! However, the Multiplication Table”. The broken version stretches the letters in “rate!” and leaves its following space narrow. Toggle `ntf_justify_kospan`. | Spaces after sentence-ending `koboSpan` elements receive a different share of justification. | Word spaces and sentence-boundary spaces expand consistently. Ignore the final, unjustified line of a paragraph. |
| `04-punctuation` | Select full justification. Inspect the dialogue's curly quotes and dashes. Toggle `ntf_justify_punct`. | Extra justification space appears around punctuation. | Punctuation stays attached to its surrounding text while word spaces expand. |
| `05-tracking` | Use left alignment. Inspect the tracked title and author line. Toggle `ntf_letterspace_spaces`. | Word spaces remain narrow beside the tracked letters. | Word spaces receive the same added tracking. Body text keeps its ordinary spacing. |
| `06-fonts` | Select a visibly different reading font. Change its size, visit all three chapters, leave and reopen the book. Repeat with a numbered family such as Source Serif 4. Toggle `ntf_kepub_fontfix` and `ntf_quote_fontfamily` separately. | A chapter falls back to the system font, the numbered family never applies, or line spacing changes after a size increase and decrease. | The selected font and line spacing survive changes and chapter loads. The font-reload defect is timing-dependent; an unreproduced baseline is not evidence that the repair ran. |
| `07-capitals` | Use PT Serif Regular, which has `cpsp`. Set `ntf_fast_shaping:0` in both runs. Toggle `ntf_cpsp_fix`; compare the capitals in Alice and White Rabbit. | Capitals have additional advance in ordinary prose. | Capital spacing follows the font's normal body-text metrics. Fonts without `cpsp` should be unchanged. |
| `09-page-edges` | Start with narrow line spacing. Read several pages forward and back; change size if no line touches a page boundary. Toggle `ntf_pagecut_trim`. | A line's top or bottom is clipped, or its ink appears on two pages. | Each line appears whole on one page. Check both sides of every affected boundary for missing or duplicated text. |
| `10-openers` | Select left alignment. Compare the first chapter, then the control chapter. Toggle `ntf_center_images` and `ntf_dropcap_fix` separately. | The ornament moves left; the oversized inline A increases the gap below its line. | The ornament stays centred and the next line uses normal spacing. The left-aligned ornament and floated A in the control chapter retain their layout. |
| `14-small-caps` | Select Libron 0.25 and `optimizeLegibility`. Compare Alice and White Rabbit with the ordinary-capitals chapter. Toggle `ntf_smallcaps`. Repeat with a font without `smcp`. | Names use scaled-down full capitals, with thinner strokes. | Names use the font's small-cap glyphs. The font without `smcp` retains stock rendering. |
| `15-long-chapter` | Open the long chapter and record its page count and several pages, including the last. Leave, reopen, then visit the short chapter. Toggle `ntf_fast_shaping`, `ntf_skip_parse_layout` and `ntf_fast_epub_delivery` separately. | The chapter may spend time shaping, performing intermediate layout and waiting between delivery chunks. | Loading completes without missing text, blank pages or wrong chapter transitions. Cache, layout skipping and delivery changes preserve output with the same shaper. NG itself can change line breaks. Do not use load time alone as a correctness test. |

For a repeatable font-loading comparison, install the static TTF files `SourceSerif4-Regular.ttf`, `SourceSerif4-Bold.ttf`, `SourceSerif4-It.ttf` and `SourceSerif4-BoldIt.ttf` from [Source Serif 4 version 4.005](https://github.com/adobe-fonts/source-serif/releases/tag/4.005R). Select Source Serif 4 at size 42, line spacing 1.0, margins 2 and full justification. Leave `ntf_quote_fontfamily:1` enabled and compare `ntf_kepub_fontfix:0` with `1`. Reboot before each run. Open `06-fonts` directly without first opening the font picker or another book, then use the table of contents to return to Chapter 1. Capture the page, increase the size once and decrease it once, then return to Chapter 1 and capture again. With the fix off, body lines change from 58 to 67 pixels apart in the reference layout. With it on, both captures are identical. Check that the heading uses Bold and the body uses Regular. This case demonstrates stale line-height metrics; it does not by itself reproduce a chapter remaining in a fallback font.

For the optional spacing slider, set `ntf_more_spacing:1`, reboot and check all 24 positions on `09-page-edges`. They run from 0.80 to 1.50. Reopening settings must retain the selected value. Set the key to `0` and reboot to return to the stock choices.

## Screenshots

The reference layout uses a 6-inch Clara screen at 1072 × 1448 pixels. Use [Libron 0.25](https://github.com/nicoverbruggen/libron) for Roman text and Kobo Tsukushi Mincho for Japanese. Start at reading size 42 and line spacing 1.0. Use [PT Serif Regular](https://github.com/google/fonts/blob/main/ofl/ptserif/PT_Serif-Web-Regular.ttf) for the capital-spacing case: Libron has no `cpsp` feature. The reference PT Serif file has SHA-256 `a4951fade06ff8f09b7673aa81ffb65a8cd409e24d3289a6dc670bc4dda2557a`; its [OFL license](https://github.com/google/fonts/blob/main/ofl/ptserif/OFL.txt) permits redistribution. Record any size changes needed to expose a page boundary. The reference Libron Regular file has SHA-256 `18995ba828982e7a70e92c6a107e182b8358b363aacac887409b8758042c490b`.

The README captures use the Clara BW layout on firmware 4.46.23836, reading size 42, margins 2 and line spacing 1.0. Clipping uses line spacing 0.80. Only the Regular PT Serif file is installed for the capital-spacing pair. The font-loading pair uses the four Source Serif 4 files listed above; the Regular file has SHA-256 `e5a4ee6a3d87bb9024796be390c6771e2a0eb1883dae25effaf57ca01668e24b` and Bold has `7cf4f4e1ad74f45058d5bc61716b82560442fbdcd9d3654d2dea96bf6c683d86`. Each pair disables only its named fix; the opener pair disables both image centring and drop-cap repair. Fast shaping is off in both versions to keep the same shaper throughout the comparison. The reading area is cropped from `(0, 80)` to `(1072, 1375)` without scaling.

| Example | Book and location | Alignment |
| --- | --- | --- |
| Hinting | `01-baseline`, first page | Left |
| Vertical glyphs | `02-vertical`, first page of 冒頭 | Left |
| Justification | `03-justification`, first page of “The Pool of Tears”, “rate! However” | Justified |
| Tracking | `05-tracking`, first page | Left |
| Font loading | `06-fonts`, first page of Chapter 1, Source Serif 4 | Justified |
| Capital spacing | `07-capitals`, first page, PT Serif | Left |
| Page clipping | `09-page-edges`, pages 2 and 3 at spacing 0.80 | Left |
| Chapter opener | `10-openers`, first page | Left |
| Small caps | `14-small-caps`, first page | Left |

The hinting pair changes glyph rasterization, but repeated glyphs were already aligned in this Libron sample. Do not count it as a reproduced baseline-wobble defect.

Capture the reading page with menus closed. Use the same crop for the original and fixed versions; do not move, retouch or rescale individual letters. Keep enough surrounding text to show the defect in context. Keep full-resolution originals when making enlarged details or difference images.

Use the chapter title or opening words to identify the location, rather than a page number alone. For clipping, include both adjacent pages. Record which feature was disabled for the baseline, especially when fast shaping can hide another defect.

## Rebuild

From the repository root:

```sh
python3 test/fixtures/build.py
python3 test/fixtures/check.py
```

The generator needs Python 3 and no additional packages or network access. It writes deterministic ZIP timestamps and includes the uncompressed EPUB mimetype entry first. `--output PATH` writes a separate copy for comparison. The generated EPUBs are kept in the repository so a tester can copy them directly.

## Text sources

- Lewis Carroll, *Alice's Adventures in Wonderland*, 1865, chapters I and II. [Source edition](https://www.gutenberg.org/ebooks/11). The English sources contain the first ten prose paragraphs of chapter I and the chapter II passage beginning “Let me see: four times five is twelve”. The justification book uses the chapter II passage. Emphasis markup is omitted; punctuation is retained.
- 夏目漱石 (Natsume Sōseki), *吾輩は猫である* (*I Am a Cat*), 1905–1906. [Japanese source text](https://www.aozora.gr.jp/cards/000148/files/789_14547.html). The fixture contains the first four paragraphs of chapter I and its later dialogue beginning “主人が水彩画を夢に見た翌日”. Ruby readings are omitted so this fixture isolates vertical glyph forms. For reading in English, see [Nick Bradley's 2025 translation, Volume One](https://www.penguin.co.nz/books/i-am-a-cat-9781784879792). The translation is not included in the fixtures.

These are public-domain works. Repeated passages, small caps, letter spacing, initials and chapter divisions are fixture formatting. The long chapter deliberately repeats the English excerpt; it is not a complete edition of the novel.
