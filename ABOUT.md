# About NickelTypeFix — how each fix works

> *This document was researched and written with the assistance of **Claude Fable 5** (Anthropic), based on disassembly of the actual device firmware. The author reviewed everything and tested it on real hardware. Earlier revisions were assisted by Claude Opus 4.8 and GPT 5.5; see the note in the README.*

NickelTypeFix corrects eleven text-rendering defects in Kobo's Qt 5.2 / QtWebKit / Monotype iType reader stack (firmware 4.x). Each fix is **independent**: it engages only if its seam is present on the running firmware, and otherwise logs and sits out. A mismatch in one fix never affects the others.

Fixes 1, 2, and 6 through 9 are **PLT hooks** (via NickelHook: it patches a library's `R_ARM_JUMP_SLOT` GOT entries, so it can intercept a *cross-library call* to an exported symbol). The justification fixes 3 and 4 and the letter-spacing fix 5 target functions that are **stripped/inlined** in the shipped binaries, leaving no symbol to hook, so they are applied as **in-memory byte patches** at startup (see [In-memory patching](#in-memory-patching)). Fixes 10 and 11 use a third technique: a short script run in the book's own frame, through the reader's own entry point for that (see [Script in the book's frame](#script-in-the-books-frame)). Nothing is written to any device library on disk; every change is made in the process's memory at boot and is gone when the mod is removed. This is interoperability bugfixing: no Kobo, Qt, or Monotype code is redistributed.

All addresses/offsets below are for the `4.6.2` WebKit/Qt build; the in-memory fixes locate their targets by **pattern**, not by absolute address, so they hold across firmware builds that keep the same instruction sequence.

## Background: two renderers, and why kepub exists

Kobo devices ship with two separate book renderers, and it matters which one is drawing your book.

Plain **epub** files are rendered by Adobe's RMSDK, a licensed third-party engine that Kobo includes for compatibility and for library books with Adobe DRM. Kobo can't do much with it, and it has barely changed in years.

Books from the Kobo store use Kobo's own format instead: **kepub** (typically `*.kepub.epub`), and sideloaded books can be converted to it with a tool like kepubify or Calibre's Kobo plugin. A kepub is an ordinary epub whose HTML has been preprocessed so that every sentence is wrapped in a `<span class="koboSpan">` with its own id. Nickel, the reader application, renders kepubs itself using its built-in browser engine (QtWebKit); a chapter really is a web page. The spans give Nickel stable anchors into the text, and those anchors are what power the kepub reader's nicer experience: precise reading-position tracking, highlights and annotations, footnote popups, reading statistics, and faster page turns.

The trade-off is the engine itself. It is a QtWebKit build from roughly 2013, frozen into the firmware, so its rendering bugs will never be fixed upstream. Font rasterization is done by Monotype's iType rather than plain FreeType, which brings quirks of its own (Fix 1). This QtWebKit-plus-iType stack is what NickelTypeFix patches; the RMSDK epub renderer is a separate world and is not touched.

One more piece of context: by default this engine lays text out on WebKit's "simple" path, which is fast but typographically basic. A hidden setting, `webkitTextRendering=optimizeLegibility`, moves it to the "complex" path, which unlocks real OpenType handling: ligatures, proper kerning, and hyphenation. The catch is that several long-standing kepub rendering bugs (broken vertical text, uneven justification) live precisely on that complex path, and they are the reason many readers leave the setting off. Fixes 2, 3, and 4 repair those bugs so the setting becomes safe to enjoy; Fixes 1 and 6 apply regardless of it. Fix 5 (letter-spacing) applies wherever a book actually uses `letter-spacing`, which WebKit lays out on the complex path on its own, independent of the setting.

## In plain language

If you're not a programmer, this section gives you the gist. The rest of the document tells the same story in full technical detail.

**How the Kobo draws a book.** As the background section explains, kepub pages are drawn by the reader's built-in browser engine, and a font renderer turns the letter shapes into pixels. Each fix corrects one specific mistake in that pipeline.

**What "hooking" means.** The mod never edits the Kobo's software on disk. When the reader starts, the mod redirects a few of its internal calls to itself. It works like mail forwarding: a letter sent from one part of the reader to another arrives at the mod first, gets corrected, and is passed on. Remove the mod and the mail goes directly again.

**What an "in-memory patch" is.** Two of the mistakes live in places that can't be intercepted that way. For those, the mod corrects a few bytes in the running copy of the code, the one loaded into memory at boot. The files on disk stay untouched, so every boot starts from the original. The mod also checks that the bytes are exactly what it expects before changing them; if a future firmware changed that code, the fix sits out and nothing is written.

**What "hinting" is (Fix 1).** Fonts can be snapped to the pixel grid to look crisper. Kobo's renderer snaps even fonts that carry no snapping instructions, and its guesswork places some letters a pixel too high or too low. That is the wobble. The fix asks for the letters unsnapped, so every letter lands exactly where the font says it should.

**One sticky note per page (Fix 2).** Every view in the reader (the book page, the dictionary popup, the browser) has a single slot for extra styling instructions. Think of it as one sticky note per view. The reader uses that note for its own instructions, such as your chosen reading font or the dictionary's text size, and the vertical-text fix needs to leave an instruction there too. An earlier version of the fix replaced the whole note, which destroyed whatever was already on it; that is why the dictionary text once turned tiny. The fix now reads the note, adds its one line, and later removes only that line.

**Capitals with too much space after them (Fix 7).** Some fonts include a feature meant for text set in ALL CAPITALS, which adds a little breathing room around each capital. The reader mistakenly uses it for ordinary text too, so a normal word starting with a capital gets an oddly wide gap after that first letter. The fix removes just that one feature from each font as it loads, for any font, so capitals sit normally again; ordinary letter spacing and kerning are left alone.

**Looking at the page before correcting it (Fixes 10 and 11).** Two of the problems can't be described as a styling rule, because the thing that has to be decided isn't written down anywhere in the page. Nothing marks a figure as "the one the author centred", or a letter as "a drop cap and not just an italic word". So for those two, the mod reads the chapter after the reader has laid it out and sets a style on the handful of elements it can identify. It uses the reader's own facility for running a script in a book, the one behind highlights and layout. It only reads the page and adjusts those elements. It adds nothing to the book, sends nothing anywhere, and never touches the book's files.

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

**The view-identity gate, and a correction.** `WebkitView` is shared by the dictionary, store and browser views, so only an injection on the reader's own view may arm the fix. The reader's view pointer is learned per book from the live objects, with a C++-ABI proof: the candidate must be a base subobject of the live `KepubBookReader`, and the vtable it points at must carry the matching `KepubBookReader` destructor entry — the plain `_ZN15KepubBookReaderD1Ev` when the candidate is the reader itself (`WebkitView` as the primary base, the layout confirmed on 4.45.23697), or the this-adjusting `_ZThn<off>_…D1Ev` thunk for a base at `+off`. If no candidate ever passes the proof, the fix sits out for the book rather than acting on a guess. Versions v0.5 through v0.7 instead *assumed* the view was the `+24` subobject, citing the existence of `_ZThn24_N15KepubBookReaderD1Ev` as proof — but that thunk belongs to a different base at `+24` (the one carrying the `contentMargins`/`setAutoHighlightStyle` overrides), and `KepubBookReaderBase::locatePages`, a genuine `WebkitView` override, has no thunk at all, which places `WebkitView` at offset 0. The wrong assumption made the gate unmatchable, so the fix armed never; recorded here so the same conclusion is not re-derived.

---

## Fix 7 — Capital spacing (cpsp) · `ntf_cpsp_fix`

**The bug.** Some fonts carry an OpenType `cpsp` (Capital Spacing) feature. It exists to add a little inter-letter room to text set in *all capitals*, and a correct engine applies it only to runs of capitals. Kobo's reader applies it to ordinary mixed-case body text, so every capital is pushed away from the letter after it, leaving a loose gap (the `D` in `Docks` is the giveaway). This is only visible with `optimizeLegibility` on, the path where the reader applies GPOS features at all.

**Mechanism.** With `optimizeLegibility`, Kobo's Qt 5.2 shapes text through its *old* HarfBuzz (`shapeTextWithHarfbuzz`), which applies a font's default-LangSys GPOS features wholesale, with no per-feature curation. `cpsp` is a GPOS single-positioning lookup that adds advance to each capital; because the old shaper enables it unconditionally, it fires on every capital in running text, not just in the all-caps runs it is meant for. (Qt's newer HarfBuzz-NG uses a curated default feature list that excludes `cpsp`, but this firmware defaults to the old shaper.) The feature also can't be gated correctly inside the shaper: `cpsp` and the also-default `case` feature share the same single-positioning code and differ only in data, so there is no per-tag point at which to skip only `cpsp`.

**The fix.** Rather than touch the shaper, drop `cpsp` from the font itself as it loads, which works for any font. `QFontDatabase::addApplicationFont` (PLT-hooked in `libnickel`, `_ZN13QFontDatabase18addApplicationFontERK7QString`) is the call Kobo's `FontManager` uses to register every reader font: core, system, and sideloaded. The hook reads the font file, walks its GPOS `FeatureList`, and for each `cpsp` feature sets that Feature table's `LookupIndexCount` to `0`, so the feature applies no lookups. That is a bounds-checked two-byte edit per feature: no table re-serialization, and `case`, `kern`, and every other feature are left byte-for-byte intact. The edited bytes are then registered with `QFontDatabase::addApplicationFontFromData`. It works because the old shaper reads the default-LangSys features straight from the font, so an empty `cpsp` is simply a no-op.

Fail-safe throughout: a font with no GPOS table, no `cpsp`, an unreadable path (a Qt resource, say), a malformed table, or any allocation failure falls through to the real `addApplicationFont` with the original file, so a font always loads. Only fonts actually changed take the from-data path, so the blast radius is minimal. Validated in the offscreen render harness (the loose `Docks` `D→o` gap goes from `21px` to `15px`, while the `case`-driven hyphen raise and `Va` kerning stay intact) and confirmed on-device, where the hook fires and strips `cpsp` from both a sideloaded font and one of Kobo's own. It always strips regardless of `optimizeLegibility`, though the visible effect only appears when `optimizeLegibility` is on. The symbol is `optional`; if it isn't present the fix sits out.

---

## Fix 8 — Reader-font family quoting · `ntf_quote_fontfamily`

**The bug.** A reading font whose family name has a whitespace-separated token that starts with a digit (`Source Serif 4`, `Helvetica 75`, `Bitter 24pt`) silently doesn't apply: the text draws in the default font instead, and paging does not fix it. The usual workaround is to rename the font so the number goes away.

**Mechanism.** When Kobo applies your reading font to a kepub, `KepubBookReader::pageStyleCss` builds a rule from a fixed template, `* { font-family: %1 !important; }`, and substitutes the raw family name into `%1` with no quotes. For `Source Serif 4` that yields `font-family: Source Serif 4 !important`, which is invalid CSS: an unquoted `font-family` value is a list of identifiers, and an identifier cannot begin with a digit, so the token `4` is illegal and WebKit drops the whole declaration. The font itself is registered correctly (Kobo loads it into `QFontDatabase` under its true family), so this is purely a quoting oversight. The reader even quotes a hardcoded sibling rule (`rt { font-family: 'Sans-SerifJP' !important; }`) a few bytes away; it just never quotes the dynamic reader rule.

**The fix.** The injected reader CSS flows through `WebkitView::addCssToHtml` (already hooked, `_ZN10WebkitView12addCssToHtmlE7QString`). Before the original runs, the hook scans the stylesheet for each `font-family: <value> !important` declaration and, when the value is not already quoted, wraps it in double quotes, so `Source Serif 4` becomes `"Source Serif 4"` and applies. It leaves already-quoted values alone (so the hardcoded `'Sans-SerifJP'` rule is untouched), skips comma-separated fallback lists and bare CSS generics (`serif`, `sans-serif`, `monospace`, and so on) which have to stay unquoted, and is idempotent. If the toggle is off or nothing matches, the stylesheet passes through byte-for-byte, and the whole pass runs inside the hook's existing exception guard.

---

## Fix 9 — kepub page-boundary clipping · `ntf_pagecut_trim`

**The bug.** Depending on the font, the font size and the line spacing, a line of text at a page edge comes out sliced: a page starts with flattened ascenders or a clipped question mark, or ends with cut descenders. The missing ink is not lost. It is painted on the neighbouring page instead, as a thin strip of glyph tops along the bottom of the page before.

**Mechanism.** The kepub reader builds its page table from one rectangle per line of text. `WebkitView::locatePages` collects the run rects, sorts them (`WebkitView::sortRectsByStart`), merges adjacent ones, and walks them page by page. The walk has two ways to place a page break:

- at the **top** of the line that would overhang the page. That line goes whole to the next page and nothing is cut.
- at the **bottom** of the previous rect. Whatever of the next line sits above that point stays on this page, so this is the mode that slices a line in half.

The walk only takes the second one when two rects overlap, which sounds like a rare accident. It is not, because the rects always overlap. The reader floors every line box at a minimum line height derived from `QFontMetrics::height()`, which is ascent plus descent and so leaves out the few pixels of slack the layout adds below the baseline, and it takes that measurement at a hard-coded 12 px instead of the size the page is laid out at. Measured through the device's own Qt, QtWebKit and iType, the reported height at 12 px comes out a pixel or two under the real line box, so the derived ratio lands below the box's true ratio, and the 1.3 floor the reader also applies does not clear it either. Measuring at 12 px compounds it, because one pixel there is a twelfth of an em and the per-size roundings do not scale linearly. Every line box therefore ends up a couple of pixels taller than the distance to the next one, every pair of lines overlaps, and the slicing mode is the normal case rather than the exception.

**The fix.** In the same `sortRectsByStart` hook, before the walk reads the vector, each line box is trimmed so its end cannot pass the top of the next box that starts strictly below it. That removes the overlap, and with no overlap left the walk has nothing to select its slicing mode with, so it places every break at the top of the overhanging line. In the offline harness (three fonts, two pages, two to three sizes each) the break moves by about 2 px, no line changes page, page counts are unchanged, and page-bottom white space stays within a pixel of stock.

The trim measures nothing about the font: no ink measurement, no `hhea` numbers, no font size, no conformance window. That is its reason to exist, because it then covers publisher-default books, Kobo's built-in fonts, and fonts whose files cannot be parsed. Boxes that end exactly on the next box's top are safe, because `mergeAdjacentRects` merges on containment or on a shared edge, never on touching (decoded at `0xbcd286`).

The guards all leave the geometry alone when in doubt. Boxes sharing a top are runs on one line and never trim each other. A trim may remove at most a quarter of a box's height, so a vertically shifted run on the same line (a footnote superscript, an inline image starting a few pixels below the line top) cannot collapse its line's box. A trim that would leave a zero or negative height is skipped.

One more guard, added after the trim mis-fired on a device. Trimming a box to the top of the next one assumes the next one is a line, so that stopping there stops in the gap between two lines. A floated drop cap breaks that assumption: its box begins part-way up the line beside it and is several times as tall, and trimming to its top cuts the line short of its own letters. Measured on the device, a 76 px heading box was trimmed to 58 px against a 209 px drop cap box; the page then ended at 2510 while the heading's ink ran to 2528, so its descenders printed at the top of the next page. Two boxes that really are consecutive lines have similar heights, so the trim now also refuses a pair whose heights differ by more than half. The same book after the change ends the page at 2528 with the heading whole. The older quarter-of-the-height guard asks whether the trim is too big; this one asks whether the next box is a line at all, and both have to pass.

The whole pass sits out unless it is the reader's own view (the same identity proof Fix 6 uses), the writing direction is horizontal, and the call is on the GUI thread; trimming only changes heights, so the sort order the following merge relies on is untouched. The per-pass arming is opened by the outermost `locatePages` frame and survives nested ones: a settings-change pass re-enters `locatePages` from inside itself (the writing-direction path re-renders the chapter, whose load path repaginates), and every sort of that pass has to be treated the same way, or the last walk to run decides the page table by chance. Device logs caught exactly that before release, an inner frame's exit disarming the still-running outer pass, whose untrimmed walk then overwrote the corrected page table.

**What it cannot fix.** The break now lands at the top of a line's box, so ink that rises above that box in the first place is still shaved, exactly as stock already shaved it. Fonts whose accented capitals reach past their own declared ascent do exactly that, and no rect-level fix can move them. The "never worse than stock" result is measured for ink that stays inside its box. Devanagari's deep vowel marks are the case that could differ, by 1 to 3 px, and that has not been measured.

---

## Fix 10 — Centred images under a left text alignment · `ntf_center_images`

**The bug.** Set the reader's text alignment to left, or to justified, and a figure the book centred slides to the left margin. Set it back to the publisher's default and the figure is centred again.

**Mechanism.** The alignment setting is applied as an injected rule, `div, p { text-align: <setting> !important; }`. An image alone in a block has no alignment of its own: it is inline content, so the block's `text-align` decides where it sits. The injected rule outranks whatever the book's stylesheet said about that block, so an author's `text-align: center` loses and the image travels left with the text. The book is styled correctly; the reader's own rule is simply too broad.

**The fix.** Put the author's centring back, on the blocks that had it and no others. The script (see [Script in the book's frame](#script-in-the-books-frame)) walks the images in the chapter, climbs out of the koboSpans the reader wraps content in to reach the real block, and requires that block to hold the image and nothing else: one element child, no text. It then asks what the *book* said about that block, reading the matched rules with `getMatchedCSSRules` and skipping the reader's own injected selector. Where the author asked for centre, it centres the image three ways: `text-align: center` on the block, and `display: block` with auto left and right margins on the image itself, each as an inline style with `important` priority.

Three ways because which one bites depends on the image's own box, and only one of them is enough on its own. An earlier version set `text-align` alone, deliberately, to avoid the reflow that `display: block` causes. On device that turned out not to move the image at all: the log showed the matcher finding the image and the styles applying, and the figure stayed at the left margin. `text-align` positions inline content, so it does nothing to an image whose box is not inline, and auto margins are what move that one. Setting all three covers both without needing to know which case a book is.

The reflow is not a problem any more because of *when* the script runs. Both scripts run from `KepubBookReaderBase::loadFinished`, before it chains to the real function, and the order probe shows `locatePages` running one step inside that call. Anything the script changes is therefore in place before the page table is built. This is the same seam change that finished Fix 11, and it is what allowed the working centring method to come back.

One guard beyond the author's intent: an image that already sits centred is left alone, measured by comparing the gap on each side of the image inside its block and skipping when they match within 2 px. Intent alone is not enough, because when the reader is not overriding the book the image is already where it belongs and rewriting it would reflow the text below it for nothing. An image the author left-aligned, or never styled at all, is left alone; so is one that shares its block with text. Each image is marked once and skipped afterwards. Confirmed on device.

---

## Fix 11 — Oversized drop caps · `ntf_dropcap_fix`

**The bug.** A chapter that opens with a large initial letter in an inline `<span>` pushes the line beneath it down. Measured on device, the first line box is 88 px where the rest of the paragraph runs at 69 px, so the opening two lines sit visibly further apart than every pair after them.

**Mechanism.** A drop cap done this way is ordinary inline text at several times the body size, so it takes part in its line box, and a line box grows to fit the tallest inline box on the line. Only the first line is affected, which is what makes the gap read as a mistake rather than as the design.

**The fix.** The span is clamped to `display: inline-block` with a height and a line-height of one em, which takes it out of the line-box maximum while leaving the text wrap alone. Floating it would work too, but a float indents the lines beside it and changes the design the author chose. The script only touches a paragraph's first element child, a `span` that is not a koboSpan, holding at most two characters, whose computed font size is more than 1.6 times the paragraph's.

**A floated drop cap is never touched.** A book that floats its drop cap has already solved this: a float is taken out of the line box, so it cannot push the next line down and there is nothing to correct. Clamping one actively breaks it. `float` computes `display` to `block`, so the `inline-block` above is discarded while the `height` still applies, and the float collapses to one line tall with the second line of text running through the letter. Seen on a test book whose drop cap uses the 290 % floated rule Jade War ships: the cap shrank to about a line and a half and the word beneath it collided with the stem. The script now skips any candidate whose computed `float` is not `none`, and any that is positioned. A second case in the same book, floated with `line-height: 1`, survived the broken build only because its line height already matched the height being forced on it, which made the clamp close to a no-op; the guard covers both.

**Where it runs, which is the whole difficulty.** Clamping the drop cap makes the paragraph shorter. Do that after the reader has worked out where its pages end and the view is positioned against a page table that no longer matches the document: crossing backwards into such a chapter opens mid-line, with the bottom of a row of glyphs along the top. This shipped off for a version for exactly that reason, when the script ran from `WebkitView::addCssToHtml`, which was believed at the time to be the last point before pagination.

It is not. The order probe, run on device, prints the sequence for each chapter load:

```
13  WebkitView::webkitViewLoadFinished
14  KepubBookReaderBase::loadFinished
15  WebkitView::locatePages          <- the page table
16  WebkitView::sortRectsByStart
17  WebkitView::addCssToHtml         <- where the script used to run
18  KepubBookReaderBase::loadFinished RET
```

`locatePages` runs one step inside `loadFinished`, and the CSS seam is two steps behind it. The script now runs at the top of the `loadFinished` hook, before it chains, which is ahead of pagination. In the same trace, the style writes force a synchronous relayout that itself triggers a pagination pass at 15 and 16, so the page table is built from the corrected layout rather than from the layout the script has just invalidated. That is what made the fix shippable, and it is what let Fix 10 go back to a centring method that reflows.

---

## Script in the book's frame

Fixes 10 and 11 can't be written as a CSS rule, because what has to be decided is not selectable. No selector asks whether the author centred *this* figure, or whether *this* span is a drop cap rather than an italic phrase. Both questions are answerable by looking at the laid-out document, so the mod looks. This is a third technique alongside the PLT hooks and the byte patches, and it is worth knowing that the mod can run script inside book content.

**How.** Nickel has its own entry point for running script in a book's frame, `WebkitView::evaluateJavaScriptWithBrokenness` (resolved with `nh_dlsym`, `_ZN10WebkitView32evaluateJavaScriptWithBrokennessE7QString`), and the reader itself uses it for highlights and layout. The mod builds one short script per pass and runs it through that. The symbol is `optional`; on a firmware without it, both fixes sit out. It returns a `QVariant` by value, which the call site in `WebkitView::forceLayout` (`0x00bcada4`) confirms: `r0` is the struct-return pointer, `r1` is `this`, and the caller destroys the result afterwards.

**When.** At the top of the `KepubBookReaderBase::loadFinished` hook, before it chains to the real function. This was settled on hardware rather than by assumption, with a temporary probe that numbered each step, and the answer depends on what triggered the pagination:

- **Chapter load:** `loadFinished` -> `locatePages` -> `addCssToHtml`. Pagination happens *inside* `loadFinished`, one step in, and the CSS seam trails it by two.
- **Settings change or resize:** `addCssToHtml` -> `locatePages`, with no `loadFinished` at all.

An earlier version read only the second case and ran the script from `addCssToHtml`, which is why Fix 11 shipped off: on chapter load that seam lands *after* the page table is built, so a fix that changes the paragraph's height leaves the view positioned against a table that no longer describes the document.

Running only from `loadFinished` covers the settings change too, because of what the script writes. It sets inline styles on elements in the live DOM and marks them, and a settings change re-paginates the same document without reloading it, so those styles and marks are still there and still apply at the new font size. The correction survives; it does not need re-applying. Putting a corrective pass back on the CSS seam would reintroduce exactly the bug above, since both fixes now change vertical layout.

**What it does.** It reads the chapter and sets inline styles on the elements it can identify: `text-align` on a block holding a centred image, `display` and auto side margins on the image itself, and `display`, `height` and `line-height` on a drop-cap span. That is all it writes. It adds no elements, reads nothing from outside the page, sends nothing anywhere, and never touches the book's files on disk. It still has to be idempotent, because a chapter can be loaded more than once in a session: it marks each element it touches with a `data-ntf` attribute, skips a marked element next time, and returns immediately when there is nothing to do. With both fixes and the page probe off, no script is built and none runs.

One WebKit detail matters. A style write only marks the render tree dirty; geometry is recomputed later, and pagination reads the tree straight after this call. The script therefore reads `document.body.offsetHeight` to force the recompute synchronously, but only when it actually changed something, so the common no-op pass costs nothing.

**Gates.** The pass runs on the reader's own view (the same identity proof Fix 6 uses), on the GUI thread, and only with the symbol resolved. The whole call sits inside an exception guard: an error skips that one update and logs a line, rather than propagating into Nickel.

---

## Diagnostics

Three config keys turn on extra logging. All default to `0`, all are observation only, and none of them changes anything about how a page is rendered or cut. They exist so a bug report can carry the measurements instead of a description.

- `ntf_pagecut_probe` dumps the kepub line boxes as the page walk received them, on both sides of Fix 9's trim in the same pass, names every box the trim's guards refused, and reads each placed page boundary back out of the finished page table. Every probe hook calls the real function first and hands its result back unchanged, so a boot with the probe on paginates byte-identically to the same boot with it off.
- `ntf_page_probe` writes one line describing what the chapter actually contains: how many images there are, what their parent blocks look like, and which paragraphs start with an oversized element. It was written because the Fix 10 script matched nothing on a real store kepub, and store books are converted by Kobo rather than by kepubify, so the markup nesting is not necessarily the same. Repeated identical lines collapse to the first.

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
- The hooks and lookups (Fixes 1, 2, and 6 through 11) bind exact symbols and are `optional`; a rename makes that fix inert and leaves the rest running.
- The diagnostics (`ntf_pagecut_probe`, `ntf_page_probe`) are off by default and observe only. Their hooks call the real function first and pass its result back, so a page is laid out and cut identically whether they are on or off.
- The whole mod is inert on 5.x firmware (Qt6 / Chromium; NickelHook doesn't load there).
- Logging is quiet by default: a healthy boot writes nothing to `nickel-type-fix.log`. Problems (a fix that can't apply, a failed write, a safety trip, a mistake in the config file) are always logged, and a config mistake also switches full verbose logging on for that boot. Set `ntf_log:1` to log everything, so a single boot shows which fixes engaged.
- Nothing is written to any device library on disk; a boot without the mod is stock.
