# About NickelTypeFix — how each fix works

> *This document was researched and written with the assistance of **Claude Fable 5** (Anthropic), based on disassembly of the actual device firmware. The author reviewed everything and tested it on real hardware. Earlier revisions were assisted by Claude Opus 4.8 and GPT 5.5; see the note in the README.*

NickelTypeFix corrects six text-rendering defects in Kobo's Qt 5.2 / QtWebKit / Monotype iType reader stack (firmware 4.x). Each fix is **independent**: it engages only if its seam is present on the running firmware, and otherwise logs and sits out. A mismatch in one fix never affects the others.

Three of the fixes (1, 2, and 6) are **PLT hooks** (via NickelHook: it patches a library's `R_ARM_JUMP_SLOT` GOT entries, so it can intercept a *cross-library call* to an exported symbol). The other three (the justification fixes 3 and 4, and the letter-spacing fix 5) target functions that are **stripped/inlined** in the shipped binaries, leaving no symbol to hook, so they are applied as **in-memory byte patches** at startup (see [In-memory patching](#in-memory-patching)). Nothing is written to any device library on disk; every change is made in the process's memory at boot and is gone when the mod is removed. This is interoperability bugfixing: no Kobo, Qt, or Monotype code is redistributed.

All addresses/offsets below are for the `4.6.2` WebKit/Qt build; the in-memory fixes locate their targets by **pattern**, not by absolute address, so they hold across firmware builds that keep the same instruction sequence.

## Background: two renderers, and why kepub exists

Kobo devices ship with two separate book renderers, and it matters which one is drawing your book.

Plain **epub** files are rendered by Adobe's RMSDK, a licensed third-party engine that Kobo includes for compatibility and for library books with Adobe DRM. Kobo can't do much with it, and it has barely changed in years.

Books from the Kobo store use Kobo's own format instead: **kepub** (typically `*.kepub.epub`), and sideloaded books can be converted to it with a tool like kepubify or Calibre's Kobo plugin. A kepub is an ordinary epub whose HTML has been preprocessed so that every sentence is wrapped in a `<span class="koboSpan">` with its own id. Nickel, the reader application, renders kepubs itself using its built-in browser engine (QtWebKit); a chapter really is a web page. The spans give Nickel stable anchors into the text, and those anchors are what power the kepub reader's nicer experience: precise reading-position tracking, highlights and annotations, footnote popups, reading statistics, and faster page turns.

The trade-off is the engine itself. It is a QtWebKit build from roughly 2013, frozen into the firmware, so its rendering bugs will never be fixed upstream. Font rasterization is done by Monotype's iType rather than plain FreeType, which brings quirks of its own (Fix 1). This QtWebKit-plus-iType stack is what NickelTypeFix patches; the RMSDK epub renderer is a separate world and is not touched.

One more piece of context: by default this engine lays text out on WebKit's "simple" path, which is fast but typographically basic. A hidden setting, `webkitTextRendering=optimizeLegibility`, moves it to the "complex" path, which unlocks real OpenType handling: ligatures, proper kerning, and hyphenation. The catch is that several long-standing kepub rendering bugs (broken vertical text, uneven justification) live precisely on that complex path, and they are the reason many readers leave the setting off. Fixes 2, 3, and 4 repair those bugs so the setting becomes safe to enjoy; Fixes 1 and 6 apply regardless of it. Fix 5 (letter-spacing) applies wherever a book actually uses `letter-spacing`, which WebKit lays out on the complex path on its own, independent of the setting.

## In plain language

If you're not a programmer, this section gives you the gist. The rest of the document tells the same story in full technical detail.

**How the Kobo draws a book.** As the background section explains, kepub pages are drawn by the reader's built-in browser engine, and a font renderer turns the letter shapes into pixels. Each of the seven fixes corrects one specific mistake in that pipeline.

**What "hooking" means.** The mod never edits the Kobo's software on disk. When the reader starts, the mod redirects a few of its internal calls to itself. It works like mail forwarding: a letter sent from one part of the reader to another arrives at the mod first, gets corrected, and is passed on. Remove the mod and the mail goes directly again.

**What an "in-memory patch" is.** Two of the mistakes live in places that can't be intercepted that way. For those, the mod corrects a few bytes in the running copy of the code, the one loaded into memory at boot. The files on disk stay untouched, so every boot starts from the original. The mod also checks that the bytes are exactly what it expects before changing them; if a future firmware changed that code, the fix sits out and nothing is written.

**What "hinting" is (Fix 1).** Fonts can be snapped to the pixel grid to look crisper. Kobo's renderer snaps even fonts that carry no snapping instructions, and its guesswork places some letters a pixel too high or too low. That is the wobble. The fix asks for the letters unsnapped, so every letter lands exactly where the font says it should.

**One sticky note per page (Fix 2).** Every view in the reader (the book page, the dictionary popup, the browser) has a single slot for extra styling instructions. Think of it as one sticky note per view. The reader uses that note for its own instructions, such as your chosen reading font or the dictionary's text size, and the vertical-text fix needs to leave an instruction there too. An earlier version of the fix replaced the whole note, which destroyed whatever was already on it; that is why the dictionary text once turned tiny. The fix now reads the note, adds its one line, and later removes only that line.

**Capitals with too much space after them (Fix 7).** Some fonts include a feature meant for text set in ALL CAPITALS, which adds a little breathing room around each capital. The reader mistakenly uses it for ordinary text too, so a normal word starting with a capital gets an oddly wide gap after that first letter. The fix removes just that one feature from each font as it loads, for any font, so capitals sit normally again; ordinary letter spacing and kerning are left alone.

**Why it can't brick your device.** Before doing anything at boot, the mod renames itself out of the way. Only after the reader has started successfully does it rename itself back. If a boot goes wrong, the next boot simply doesn't load the mod. Since nothing on disk is ever modified, the worst case is always the same as not having the mod installed.

---

## Fix 1 — Glyph "wobble" (hinting) · `ntf_no_hinting`

**The bug.** With certain fonts, individual letters drift up or down by a pixel, producing a visibly uneven baseline. The same fonts render cleanly on desktops / KOReader.

**Mechanism.** Kobo's rasterizer, Monotype **iType**, is registered with FreeType as a hinting-capable font driver (its module flags are `0x401` = `FT_MODULE_FONT_DRIVER | DRIVER_HAS_HINTER`). When Nickel loads a glyph *with hinting requested*, which is its default, iType grid-fits the outline. For a glyph carrying **no per-glyph instructions**, iType falls back to its own automatic grid-fitting, snapping the glyph's top to a whole pixel row; that snap is sub-pixel-position-sensitive, so the same letter lands on a different integer height depending on its horizontal position. (`gasp`/`fpgm`/`prep` are not involved; the glyph-load path doesn't read them for uninstructed glyphs.)

The call chain: `QFontEngineFT::loadGlyph` (in Kobo's QPA plugin `libkobo.so`) builds base load flags `0x200`, ORs in the glyph flag `0x8` (→ `0x208`), and calls the imported `FT_Load_Glyph` across the PLT. Inside FreeType, the load-flag mask `0x8002` (`FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_HINTING`) gates whether the driver hints; iType then either auto-gridfits (uninstructed) or runs its bytecode interpreter (instructed).

```
QFontEngineFT::loadGlyph                    (libkobo.so)
  └─▶ FT_Load_Glyph(face, index, 0x208)    ← cross-library PLT call: hooked
        [mod] flags |= FT_LOAD_NO_HINTING (0x2)  →  0x20a
  └─▶ real FT_Load_Glyph
        └─▶ iType: hinting gated off → raw scaled outline, no grid-fit
            snap → a glyph has identical geometry at every position
```

**The fix.** NickelHook hooks `FT_Load_Glyph` in `/usr/local/Kobo/platforms/libkobo.so` and, before the real call, ORs in `FT_LOAD_NO_HINTING` (`0x2`): `0x208` → `0x20a`. iType then emits the raw scaled outline with no grid-fit snap, so every instance of a glyph has identical geometry. The hook is `optional` (a missing/renamed `FT_Load_Glyph` just leaves this fix inert), and an allow-list (`ntf_hinting_allowlist`, matched case-insensitively against `FT_FaceRec::family_name`) exempts families you want natively hinted. This is orthogonal to iType's CSM stem-weighting (the Font Weight control), which is applied via `FT_Set_CSM_Adjustments` *before* the load and is unaffected.

---

## Fix 2 — Vertical (tategaki) CJK text · `ntf_vertfix`

**The bug.** With `optimizeLegibility` on (`webkitTextRendering=optimizeLegibility`), vertical Japanese/Chinese books render with the long-vowel mark `ー`, brackets `「」`, and punctuation `、。` horizontal or mislaid.

**Mechanism.** `optimizeLegibility` forces WebKit's **complex text path** (QTextLayout-based), which has no vertical-writing-mode support; WebKit's **simple path** renders tategaki correctly. The only reason a vertical book lands on the broken path is the injected `text-rendering: optimizeLegibility`. The obvious seams can't be used: `KepubBookReader::pageStyleCss` / `setWritingDirection` are C++ *virtual* methods (NickelHook can only patch PLT entries, not vtable slots), and `ReadingSettings::getWebkitTextRendering` is read *once, before* the book's writing mode is parsed, so rewriting its return is futile.

**The fix.** Override `text-rendering` on the *live page* once vertical is known. NickelHook hooks `CustomWebView::setWritingDirection` (`_ZN13CustomWebView19setWritingDirectionE16WritingDirection`, in `libnickel.so.1.0.0`), a PLT-hookable, non-virtual function that fires when a view's writing mode is applied and whose `this` exposes the page. If the direction is vertical, it pushes a **user-origin stylesheet** onto that page's `QWebSettings` via `QWebSettings::setUserStyleSheetUrl` (the settings object reached by calling `CustomWebView::settings`): a base64 `data:` URL of `*{text-rendering:auto !important}`. A user-origin `!important` rule outranks the author's `optimizeLegibility`, so WebKit re-cascades and re-renders on the simple path immediately.

**One slot, many writers.** The subtlety (verified by disassembling the firmware's `libnickel`): that slot is one QUrl per view, and the mod is **not** its only writer. `WebkitView::addCssToHtml` is literally:

```
WebkitView::addCssToHtml(css)
  └─▶ QWebSettings::setUserStyleSheetUrl(
          CustomWebView::settings(),
          StringUtil::encodeAsUrlData(css, "text/css"))   // → data:text/css;charset=utf-8;base64,…
```

and every `WebkitView`-derived view stores its own CSS through it: the book reader itself (`KepubBookReader::addCssToHtml` forwards to the base after `QWebFrame::removeCSSRule`), the dictionary, the in-app browser, and the store. `CustomWebView::setWritingDirection` also fires for all of them (via `WebkitView::setWritingDirection`/`::locatePages`).

```
                      one QUrl per view (the user-stylesheet slot)
                     ┌───────────────────────────────────────────┐
  reader font CSS ──▶│                                           │
  dictionary CSS  ──▶│  data:text/css;charset=utf-8;base64,<b64> │◀── the mod's override
  browser / store ──▶│                                           │
                     └───────────────────────────────────────────┘
                     last write wins — there is no "add one rule"
```

Blindly *clearing* the slot blanked the dictionary (its enlarged-definition CSS vanished, leaving the text unreadably small); blindly *setting* it would wipe the reader's own font CSS and be wiped right back by the next chapter injection.

**Coexistence protocol.** The override travels *inside* whatever the slot holds, so both survive. Nickel and the mod encode the slot in the same `data:text/css;charset=utf-8;base64` shape, so it decodes, edits, and re-encodes losslessly:

| event | slot before | the mod does | slot after |
|---|---|---|---|
| `setWritingDirection(vertical)` | empty | set the pure override | `rule` |
| `setWritingDirection(vertical)` | view's own CSS | decode, merge the rule in | `CSS + rule` |
| CSS injection for a vertical view | anything | append the rule to the CSS in flight | `new CSS + rule` |
| `setWritingDirection(horizontal)` | `CSS + rule` | decode, strip *only its own* rule | `CSS` (or empty) |
| `setWritingDirection(horizontal)` | view's own CSS, no rule | nothing; a foreign slot is never touched | unchanged |

The read-back that drives this (`QWebSettings::userStyleSheetUrl`, dlsym'd) means every decision is made from what the slot *actually* holds, never from remembered state alone. One quirk, observed on device: Nickel transiently applies a horizontal direction to the reader view during each chapter transition, before the chapter's writing mode is parsed. In a vertical book the rule is therefore stripped and re-merged once per chapter; both operations preserve the slot's other CSS, and the re-merge lands before the vertical mode is applied. If the read-back getter is missing on a firmware, the fix degrades to plain per-view set/clear. The vertical enum values (`vertical-rl`, `vertical-lr`) are derived at runtime by calling Nickel's own `writingDirectionFromString` (dlsym'd) rather than hardcoded, so a firmware that renumbers them still works. Every symbol here is `optional`; if one is missing this fix goes inert and the others are unaffected.

---

## Fix 3 — Justified text at koboSpan boundaries · `ntf_justify_kospan` (the main justify fix)

**The bug.** In a kepub with justification + `optimizeLegibility` on, justified lines crossing a sentence boundary get a starved space at the boundary while the line's other gaps over-stretch: an uneven line, even in plain text with no special punctuation.

**Mechanism.** kepubify wraps every sentence in its own `<span class="koboSpan">`. On the complex path WebKit draws each span as a **separate justification run** (a separate `QPainter::drawText` → `QScriptLine`). Qt's `QTextEngine::justify` distributes a run's expansion budget only over the index range `[from … from + length − 1]`, but WebCore passes each koboSpan run's **trailing space in `si.trailingSpaces`, *outside* `si.length`**. So the boundary space (the trailing space of one span, mid-line) is never in the justify range, never receives the expansion WebCore already budgeted for it, and stays at natural width; that slack is absorbed by the span's internal spaces instead.

```
one justified line crossing a sentence (koboSpan) boundary:

  [ …end of sentence one.]·[Start of sentence two… ]
  └────── span run A ────┘ └────── span run B ─────┘
                          ^
     the boundary gap is run A's TRAILING space: WebCore hands it to Qt
     in si.trailingSpaces, outside si.length, so justify's range
     [from … from+length−1] never reaches it → it stays natural width
     while run A's inner spaces absorb the stretch budgeted for it
```

**The fix.** Two byte-pairs in `QTextEngine::justify` (`libQtGui.so.4.6.2`) that make the justify range include the trailing space. Both are required (each alone is a no-op), so they're applied **both-or-nothing**:

| site | change | disasm | bytes |
|---|---|---|---|
| A | skip the trailing-trim loop | `bpl.w` → `b.w` | `40 F1 9E 80` → `00 F0 9E B8` |
| B | justify range = length, not length−1 | `subs r3,r4,#1` → `movs r3,r4` | `63 1E` → `23 00` |

Anchors (position-independent, unique in the binary):

- Site A: `15 F8 01 3C D8 06 40 F1 9E 80 04 E0`; edit the 4 bytes at **anchor + 6**.
- Site B: `2C 46 51 E7 63 1E 3B 61 DC D0`; edit the 2 bytes at **anchor + 4**.

(Note the `b.w` in Site A is Thumb T4 with S=0, J1=J2=1 → `f000 b89e`.) Validated to render byte-identical to a correct reference, with **zero regression** on normal / single-span / CJK / simple-path text. It fixes every existing kepub without re-conversion and keeps ligatures. Gated by `ntf_justify_kospan`; if either anchor isn't found (or is ambiguous, or the bytes differ), neither edit is written.

---

## Fix 4 — Justification around punctuation · `ntf_justify_punct` (secondary)

**The bug.** Justified text can space unevenly around em/en dashes, ellipses, and curly quotes.

**Mechanism.** The device build widened `WebCore::Font::isInterIdeographExpansionTarget` to return `true` for **General Punctuation U+2000–U+206F** (except the hyphens U+2010/U+2011), a range that includes the em dash `—`, en dash `–`, ellipsis `…`, and curly quotes `“ ” ‘ ’`, in addition to the CJK/symbol/fullwidth ranges it's meant for. `canExpandAroundIdeographsInComplexText` is already `true` on the device, so those codepoints get counted as justification-stretch opportunities.

**The fix.** One byte-pair in that function (`libQtWebKit.so.4.6.2`) makes the U+2000–U+206F branch return false. The function is a single out-of-line body; its in-range branch returns "is a target" via `mov r0, r3` (`18 46`), and the patch changes that to `movs r0, #0` (`00 20`). Anchor (the function prologue, the `sub #0x2000 / sub #0x10 / cmp #1 / cmp #0x6f` idiom): `a0 f5 00 52 a2 f1 10 03 01 2b 8c bf 01 23 00 23 6f 2a 88 bf 00 23 0b b1`; edit the 2 bytes at **anchor + 0x18**. Every other range (real CJK, symbols, fullwidth) is untouched. Gated by `ntf_justify_punct`. This is the secondary justify fix; Fix 3 addresses the common visible bug.

---

## Fix 5 — Letter-spacing on spaces · `ntf_letterspace_spaces`

**The bug.** CSS `letter-spacing` (tracking) widens the letters of a run but leaves the spaces, and the letter immediately before each space, at their natural width. So any multi-word letter-spaced text (a tracked heading, a styled caption, spaced small-caps) has its letters spread while its words run together. Browsers and the CSS Text spec add the tracking to spaces too.

**Mechanism.** `QTextEngine::shapeText` (`libQtGui.so.4.6.2`) first adds `letterSpacing` to *every* glyph's advance, spaces included. It then runs a separate word/space loop that, for each space glyph, **subtracts** `letterSpacing` back off the space *and* the glyph before it, then adds `wordSpacing` to the space:

```
shapeText: advances_x[i] += letterSpacing   for every glyph i   (spaces included)
then, for each glyph k whose HB_GlyphAttributes.justification is a space:
    advances_x[k]   -= letterSpacing     <- the space loses its tracking
    advances_x[k-1] -= letterSpacing     <- the letter before it loses its tracking
    advances_x[k]   += wordSpacing
```

That subtraction is what leaves the word gaps narrow, and it is **not** stock Qt: a source review of `QTextEngine::shapeText` from 4.6.2 through 6.x shows the space-handling block only *adds* `wordSpacing` to a run of spaces and never subtracts `letterSpacing`. The withholding is therefore specific to the QtEmbedded / iType binary Kobo freezes into the firmware, not upstream behavior, so no Qt release fixes it and a byte patch is the route. (The CSS WG did accept a spec-correct "track spaces too" model, [csswg-drafts#10193](https://github.com/w3c/csswg-drafts/issues/10193), but that is about the model, not this binary.) The drawn text really does come from this path: patching the add-loop alone changes nothing, while patching these two subtracts changes the drawn advances, confirmed by rendering under a debugger.

**The fix.** Two byte-pairs in that loop (`libQtGui.so.4.6.2`) that `nop.w` the two subtracts, so spaces and the pre-space letter keep the tracking `shapeText` already gave them. `wordSpacing` (added right after) is untouched, and each subtract is `advances -= letterSpacing`, a no-op when there is no letter-spacing, so ordinary text is byte-identical.

| site | change | disasm | bytes |
|---|---|---|---|
| A | space keeps its tracking | `rsb r3,sl,r3` → `nop.w` | `ca eb 03 03` → `af f3 00 80` |
| B | pre-space letter keeps its tracking | `rsbne r5,sl,r5` → `nop.w` | `ca eb 05 05` → `af f3 00 80` |

Anchor (position-independent, unique in the binary), both edits at one site:

- `43 68 18 bf 05 68 ca eb 03 03 18 bf ca eb 05 05 43 60 18 bf 05 60` (22 bytes); edit A at **anchor + 6**, edit B at **anchor + 12**.

Both required, applied **both-or-nothing**. Validated in the offscreen render harness: for a letter-spaced title at `0.5em` (`+23px`), the drawn advances go from tracking every glyph *except* the spaces and the letter before each space, to tracking all of them (each space `12px → 35px`, each pre-space letter regaining its `+23px`), with `word-spacing` still landing on top. Zero regression on text without letter-spacing (the subtract is a no-op there; a plain justified page renders identically). Gated by `ntf_letterspace_spaces`; if the anchor isn't found (or is ambiguous, or the bytes differ), neither edit is written.

---

## Fix 6 — Reader-font fallback repair · `ntf_kepub_fontfix`

**The bug.** In a kepub book, a chapter's text sometimes renders in the system (fallback) font instead of the chosen reading font, and stays that way on page turns; only changing the font or reopening the book clears it.

**Mechanism.** The reading font is applied as an injected `* { font-family:'<font>' !important; }` rule (`KepubBookReader::pageStyleCss` → `addCssToHtml`), resolved against a `QFontDatabase` application font. If the font isn't ready the instant a chapter first draws, WebKit resolves the family to a substitute. Nothing on a plain page turn re-runs the cascade, so the chapter stays stuck on the fallback.

**The fix.** Re-apply the reader-font rule once per chapter, on an arm/consume rhythm:

```
chapter loads
  ├─▶ WebkitView::addCssToHtml(font CSS)   [hook: ARM — the per-chapter,
  │                                          font-agnostic "fresh chapter drew" signal]
  └─▶ WebkitView::setCurrentPage(n)        [hook: CONSUME — rebuild the font rule
                                             (pageStyleCss) and re-inject it
                                             → WebKit re-resolves the font in place]
page turns within the chapter: nothing armed → nothing done
```

`WebkitView::addCssToHtml` (PLT-hooked, `_ZN10WebkitView12addCssToHtmlE7QString`) fires when a chapter injects its font CSS, which arms the fix; the next `WebkitView::setCurrentPage` (`_ZN10WebkitView14setCurrentPageEi`) consumes it: it calls `KepubBookReader::pageStyleCss` to rebuild the rule and `KepubBookReader::addCssToHtml` (both dlsym'd), which removes the old frame rule (`QWebFrame::removeCSSRule`) and re-sets the page's user stylesheet through the base `WebkitView::addCssToHtml`, so WebKit re-cascades and re-resolves the font in place. This is the same re-inject the reader itself runs on a font size/family change (`applyStyling`), minus the repaginate, so the reading position doesn't move. On an already-correct chapter it renders the identical font and is invisible. A re-entrancy guard keeps the fix's own re-inject from re-arming it, and hooking the `KepubBookReader` constructor (`_ZN15KepubBookReaderC1EP11PluginStateP7QWidget`) resets the per-book state. Every symbol here is `optional`; a missing one sits the fix out. This fix is independent of `optimizeLegibility` and only affects kepub books.

---

## Fix 7 — Capital spacing (cpsp) · `ntf_cpsp_fix`

**The bug.** Some fonts carry an OpenType `cpsp` (Capital Spacing) feature. It exists to add a little inter-letter room to text set in *all capitals*, and a correct engine applies it only to runs of capitals. Kobo's reader applies it to ordinary mixed-case body text, so every capital is pushed away from the letter after it, leaving a loose gap (the `D` in `Docks` is the giveaway). This is only visible with `optimizeLegibility` on, the path where the reader applies GPOS features at all.

**Mechanism.** With `optimizeLegibility`, Kobo's Qt 5.2 shapes text through its *old* HarfBuzz (`shapeTextWithHarfbuzz`), which applies a font's default-LangSys GPOS features wholesale, with no per-feature curation. `cpsp` is a GPOS single-positioning lookup that adds advance to each capital; because the old shaper enables it unconditionally, it fires on every capital in running text, not just in the all-caps runs it is meant for. (Qt's newer HarfBuzz-NG uses a curated default feature list that excludes `cpsp`, but this firmware defaults to the old shaper.) The feature also can't be gated correctly inside the shaper: `cpsp` and the also-default `case` feature share the same single-positioning code and differ only in data, so there is no per-tag point at which to skip only `cpsp`.

**The fix.** Rather than touch the shaper, drop `cpsp` from the font itself as it loads, which works for any font. `QFontDatabase::addApplicationFont` (PLT-hooked in `libnickel`, `_ZN13QFontDatabase18addApplicationFontERK7QString`) is the call Kobo's `FontManager` uses to register every reader font: core, system, and sideloaded. The hook reads the font file, walks its GPOS `FeatureList`, and for each `cpsp` feature sets that Feature table's `LookupIndexCount` to `0`, so the feature applies no lookups. That is a bounds-checked two-byte edit per feature: no table re-serialization, and `case`, `kern`, and every other feature are left byte-for-byte intact. The edited bytes are then registered with `QFontDatabase::addApplicationFontFromData`. It works because the old shaper reads the default-LangSys features straight from the font, so an empty `cpsp` is simply a no-op.

Fail-safe throughout: a font with no GPOS table, no `cpsp`, an unreadable path (a Qt resource, say), a malformed table, or any allocation failure falls through to the real `addApplicationFont` with the original file, so a font always loads. Only fonts actually changed take the from-data path, so the blast radius is minimal. Validated in the offscreen render harness (the loose `Docks` `D→o` gap goes from `21px` to `15px`, while the `case`-driven hyphen raise and `Va` kerning stay intact) and confirmed on-device, where the hook fires and strips `cpsp` from both a sideloaded font and one of Kobo's own. It always strips regardless of `optimizeLegibility`, though the visible effect only appears when `optimizeLegibility` is on. The symbol is `optional`; if it isn't present the fix sits out.

---

## In-memory patching

Fixes 3, 4, and 5 target functions with no exported symbol, so they can't use `nh_hook`/`nh_dlsym`. Instead, at NickelHook `init` the mod:

1. **Locates the loaded library** with `dl_iterate_phdr`, matching the object by name (e.g. contains `Gui`, or `WebKit` but not `Widgets`) and taking its executable `PT_LOAD` segment. If the lib isn't mapped yet it is `dlopen`'d first.
2. **Pattern-scans** that segment for the fix's position-independent **anchor** byte sequence, accumulating matches across all matching objects.
3. **Verifies** the exact expected original bytes at `match + offset` (and treats an already-patched site as done).
4. **Writes** the patch: require the 2- or 4-byte Thumb instruction to be naturally aligned, temporarily add write permission while keeping the page executable, replace it with one atomic store, verify the bytes, flush the instruction cache, and restore the segment's original permissions. Nickel already has several threads at plugin initialization, so keeping execute permission avoids faulting an unrelated function which happens to share the page; the atomic store prevents a partially written instruction from being observed if a target is reached unexpectedly.

A fix's edits are all located and verified before *any* is written (both-or-nothing). Anything unexpected (pattern not found, more than one match, or wrong bytes) makes that fix log and leave the library untouched. If a write fails, every changed site is rolled back and verified; an unverifiable rollback invokes the firmware's normal reboot command while NickelHook's boot failsafe is still armed, with the kernel reboot syscall as a fallback.

## Firmware tolerance & safety

- The in-memory anchors (Fixes 3, 4, 5) were verified present and byte-identical in real 4.38 and 4.45 firmware `libQtGui`/`libQtWebKit`, even though those libraries otherwise diverge (the letter-spacing anchor sits at `0x1303bc` on 4.38 vs `0x130854` on 4.45, found by the same pattern), so the same patches hold across the device line. All are located by pattern, so if a future build re-encodes the target, the anchor simply won't match and the fix sits out.
- The hooks (Fixes 1, 2, and 6) bind exact symbols and are `optional`; a rename makes that fix inert.
- The whole mod is inert on 5.x firmware (Qt6 / Chromium; NickelHook doesn't load there).
- Logging is quiet by default: a healthy boot writes nothing to `nickel-type-fix.log`. Problems (a fix that can't apply, a failed write, a safety trip, a mistake in the config file) are always logged, and a config mistake also switches full verbose logging on for that boot. Set `ntf_log:1` to log everything, so a single boot shows which fixes engaged.
- Nothing is written to any device library on disk; a boot without the mod is stock.
