# About NickelTypeFix — how each fix works

NickelTypeFix corrects five text-rendering defects in Kobo's Qt 5.2 / QtWebKit / Monotype iType reader stack (firmware 4.x). Each fix is **independent** — it engages only if its seam is present on the running firmware and otherwise logs and sits out, so a mismatch on one never affects the others.

Three of the fixes (1, 2, and 5) are **PLT hooks** (via NickelHook: it patches a library's `R_ARM_JUMP_SLOT` GOT entries, so it can intercept a *cross-library call* to an exported symbol). The other two (the justification fixes, 3 and 4) target functions that are **stripped/inlined** in the shipped binaries — there is no symbol to hook — so they are applied as **in-memory byte patches** at startup (see [In-memory patching](#in-memory-patching)). Nothing is written to any device library on disk; every change is made in the process's memory at boot and is gone when the mod is removed. This is interoperability bugfixing: no Kobo, Qt, or Monotype code is redistributed.

All addresses/offsets below are for the `4.6.2` WebKit/Qt build; the in-memory fixes locate their targets by **pattern**, not by absolute address, so they hold across firmware builds that keep the same instruction sequence.

---

## Fix 1 — Glyph "wobble" (hinting) · `ntf_no_hinting`

**The bug.** With certain fonts, individual letters drift up or down by a pixel, producing a visibly uneven baseline. The same fonts render cleanly on desktops / KOReader.

**Mechanism.** Kobo's rasterizer, Monotype **iType**, is registered with FreeType as a hinting-capable font driver (its module flags are `0x401` = `FT_MODULE_FONT_DRIVER | DRIVER_HAS_HINTER`). When Nickel loads a glyph *with hinting requested* — its default — iType grid-fits the outline. For a glyph carrying **no per-glyph instructions**, iType falls back to its own automatic grid-fitting, snapping the glyph's top to a whole pixel row; that snap is sub-pixel-position-sensitive, so the same letter lands on a different integer height depending on its horizontal position. (`gasp`/`fpgm`/`prep` are not involved — the glyph-load path doesn't read them for uninstructed glyphs.)

The call chain: `QFontEngineFT::loadGlyph` (in Kobo's QPA plugin `libkobo.so`) builds base load flags `0x200`, ORs in the glyph flag `0x8` (→ `0x208`), and calls the imported `FT_Load_Glyph` across the PLT. Inside FreeType, the load-flag mask `0x8002` (`FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_HINTING`) gates whether the driver hints; iType then either auto-gridfits (uninstructed) or runs its bytecode interpreter (instructed).

**The fix.** NickelHook hooks `FT_Load_Glyph` in `/usr/local/Kobo/platforms/libkobo.so` and, before the real call, ORs in `FT_LOAD_NO_HINTING` (`0x2`): `0x208` → `0x20a`. iType then emits the raw scaled outline with no grid-fit snap, so every instance of a glyph has identical geometry. The hook is `optional` (a missing/renamed `FT_Load_Glyph` just leaves this fix inert), and an allow-list (`ntf_hinting_allowlist`, matched case-insensitively against `FT_FaceRec::family_name`) exempts families you want natively hinted. This is orthogonal to iType's CSM stem-weighting (the Font Weight control), which is applied via `FT_Set_CSM_Adjustments` *before* the load and is unaffected.

---

## Fix 2 — Vertical (tategaki) CJK text · `ntf_vertfix`

**The bug.** With `optimizeLegibility` on (`webkitTextRendering=optimizeLegibility`), vertical Japanese/Chinese books render with the long-vowel mark `ー`, brackets `「」`, and punctuation `、。` horizontal or mislaid.

**Mechanism.** `optimizeLegibility` forces WebKit's **complex text path** (QTextLayout-based), which has no vertical-writing-mode support; WebKit's **simple path** renders tategaki correctly. The only reason a vertical book lands on the broken path is the injected `text-rendering: optimizeLegibility`. The obvious seams can't be used: `KepubBookReader::pageStyleCss` / `setWritingDirection` are C++ *virtual* methods (NickelHook can only patch PLT entries, not vtable slots), and `ReadingSettings::getWebkitTextRendering` is read *once, before* the book's writing mode is parsed, so rewriting its return is futile.

**The fix.** Override `text-rendering` on the *live page* once vertical is known. NickelHook hooks `CustomWebView::setWritingDirection` (`_ZN13CustomWebView19setWritingDirectionE16WritingDirection`, in `libnickel.so.1.0.0`) — a PLT-hookable, non-virtual function that fires when a view's writing mode is applied and whose `this` exposes the page. If the direction is vertical, it pushes a **user-origin stylesheet** onto that page's `QWebSettings` via `QWebSettings::setUserStyleSheetUrl` (the settings object reached by calling `CustomWebView::settings`): a base64 `data:` URL of `*{text-rendering:auto !important}`. A user-origin `!important` rule outranks the author's `optimizeLegibility`, so WebKit re-cascades and re-renders on the simple path immediately. The subtlety (verified by disassembling the firmware's `libnickel`): that slot is one QUrl per view and is **not** the mod's alone. `WebkitView::addCssToHtml` is literally `setUserStyleSheetUrl(settings(), StringUtil::encodeAsUrlData(css, "text/css"))`, and every `WebkitView`-derived view stores its own CSS there — the book reader itself (whose `KepubBookReader::addCssToHtml` forwards to the base after `QWebFrame::removeCSSRule`), the dictionary, the in-app browser, and the store; `CustomWebView::setWritingDirection` also fires for all of them (via `WebkitView::setWritingDirection`/`::locatePages`). Blindly *clearing* the slot blanked the dictionary (its enlarged-definition CSS vanished, leaving the text unreadably small); blindly *setting* it would wipe the reader's own font CSS and be wiped right back by the next chapter injection. So the override **coexists** with the slot's owner instead of competing: CSS injections bound for a view known to be vertical get the rule appended in flight (inside the already-hooked `WebkitView::addCssToHtml`), and on each writing-direction change the mod reads the slot back (`QWebSettings::userStyleSheetUrl`, dlsym'd) and repairs it — an empty slot gets the pure override, existing CSS gets the rule merged in (Nickel's slot URLs use the same `data:text/css;charset=utf-8;base64` format, so they decode and re-encode losslessly), and when a view goes horizontal only the mod's own rule is stripped back out. (Nickel transiently applies a horizontal direction to the reader view during each chapter transition before the chapter's writing mode is parsed — observed on device — so in a vertical book the rule is stripped and re-merged once per chapter; both operations preserve the slot's other CSS, and the re-merge lands before the vertical mode is applied.) If the read-back getter is missing on a firmware, the fix degrades to plain per-view set/clear. The vertical enum values (`vertical-rl`, `vertical-lr`) are derived at runtime by calling Nickel's own `writingDirectionFromString` (dlsym'd) rather than hardcoded, so a firmware that renumbers them still works. Every symbol here is `optional`; if one is missing this fix goes inert and the others are unaffected.

---

## Fix 3 — Justified text at koboSpan boundaries · `ntf_justify_kospan` (the main justify fix)

**The bug.** In a kepub with justification + `optimizeLegibility` on, justified lines crossing a sentence boundary get a starved space at the boundary while the line's other gaps over-stretch — an uneven line, even in plain text with no special punctuation.

**Mechanism.** kepubify wraps every sentence in its own `<span class="koboSpan">`. On the complex path WebKit draws each span as a **separate justification run** (a separate `QPainter::drawText` → `QScriptLine`). Qt's `QTextEngine::justify` distributes a run's expansion budget only over the index range `[from … from + length − 1]` — but WebCore passes each koboSpan run's **trailing space in `si.trailingSpaces`, *outside* `si.length`**. So the boundary space (the trailing space of one span, mid-line) is never in the justify range, never receives the expansion WebCore already budgeted for it, and stays at natural width; that slack is absorbed by the span's internal spaces instead.

**The fix.** Two byte-pairs in `QTextEngine::justify` (`libQtGui.so.4.6.2`) that make the justify range include the trailing space. Both are required — each alone is a no-op — so they're applied **both-or-nothing**:

| site | change | disasm | bytes |
|---|---|---|---|
| A | skip the trailing-trim loop | `bpl.w` → `b.w` | `40 F1 9E 80` → `00 F0 9E B8` |
| B | justify range = length, not length−1 | `subs r3,r4,#1` → `movs r3,r4` | `63 1E` → `23 00` |

Anchors (position-independent, unique in the binary):

- Site A: `15 F8 01 3C D8 06 40 F1 9E 80 04 E0` — edit the 4 bytes at **anchor + 6**.
- Site B: `2C 46 51 E7 63 1E 3B 61 DC D0` — edit the 2 bytes at **anchor + 4**.

(Note the `b.w` in Site A is Thumb T4 with S=0, J1=J2=1 → `f000 b89e`.) Validated to render byte-identical to a correct reference, with **zero regression** on normal / single-span / CJK / simple-path text. It fixes every existing kepub without re-conversion and keeps ligatures. Gated by `ntf_justify_kospan`; if either anchor isn't found (or is ambiguous, or the bytes differ), neither edit is written.

---

## Fix 4 — Justification around punctuation · `ntf_justify_punct` (secondary)

**The bug.** Justified text can space unevenly around em/en dashes, ellipses, and curly quotes.

**Mechanism.** The device build widened `WebCore::Font::isInterIdeographExpansionTarget` to return `true` for **General Punctuation U+2000–U+206F** (except the hyphens U+2010/U+2011) — em dash `—`, en dash `–`, ellipsis `…`, curly quotes `“ ” ‘ ’` — in addition to the CJK/symbol/fullwidth ranges it's meant for. `canExpandAroundIdeographsInComplexText` is already `true` on the device, so those codepoints get counted as justification-stretch opportunities.

**The fix.** One byte-pair in that function (`libQtWebKit.so.4.6.2`) makes the U+2000–U+206F branch return false. The function is a single out-of-line body; its in-range branch returns "is a target" via `mov r0, r3` (`18 46`) — change it to `movs r0, #0` (`00 20`). Anchor (the function prologue, the `sub #0x2000 / sub #0x10 / cmp #1 / cmp #0x6f` idiom): `a0 f5 00 52 a2 f1 10 03 01 2b 8c bf 01 23 00 23 6f 2a 88 bf 00 23 0b b1` — edit the 2 bytes at **anchor + 0x18**. Every other range (real CJK, symbols, fullwidth) is untouched. Gated by `ntf_justify_punct`. This is the secondary justify fix; Fix 3 addresses the common visible bug.

---

## Fix 5 — Reader-font fallback repair · `ntf_kepub_fontfix`

**The bug.** In a kepub book, a chapter's text sometimes renders in the system (fallback) font instead of the chosen reading font, and stays that way on page turns — only changing the font, or reopening the book, clears it.

**Mechanism.** The reading font is applied as an injected `* { font-family:'<font>' !important; }` rule (`KepubBookReader::pageStyleCss` → `addCssToHtml`), resolved against a `QFontDatabase` application font. If the font isn't ready the instant a chapter first draws, WebKit resolves the family to a substitute — and nothing on a plain page turn re-runs the cascade, so the chapter stays stuck on the fallback.

**The fix.** Re-apply the reader-font rule once per chapter. `WebkitView::addCssToHtml` (PLT-hooked, `_ZN10WebkitView12addCssToHtmlE7QString`) fires when a chapter injects its font CSS — a per-chapter, font-agnostic "a fresh chapter drew" signal — which arms the fix; the next `WebkitView::setCurrentPage` (`_ZN10WebkitView14setCurrentPageEi`) consumes it: it calls `KepubBookReader::pageStyleCss` to rebuild the rule and `KepubBookReader::addCssToHtml` (both dlsym'd), which removes the old frame rule (`QWebFrame::removeCSSRule`) and re-sets the page's user stylesheet through the base `WebkitView::addCssToHtml`, so WebKit re-cascades and re-resolves the font in place. This is the same re-inject the reader itself runs on a font size/family change (`applyStyling`), minus the repaginate — the reading position doesn't move, and on an already-correct chapter it renders the identical font, so it is invisible. A re-entrancy guard keeps the fix's own re-inject from re-arming it, and hooking the `KepubBookReader` constructor (`_ZN15KepubBookReaderC1EP11PluginStateP7QWidget`) resets the per-book state. Every symbol here is `optional`; a missing one sits the fix out. This fix is independent of `optimizeLegibility` and only affects kepub books.

---

## In-memory patching

Fixes 3–4 target functions with no exported symbol, so they can't use `nh_hook`/`nh_dlsym`. Instead, at NickelHook `init` the mod:

1. **Locates the loaded library** with `dl_iterate_phdr`, matching the object by name (e.g. contains `Gui`, or `WebKit` but not `Widgets`) and taking its executable `PT_LOAD` segment. If the lib isn't mapped yet it is `dlopen`'d first.
2. **Pattern-scans** that segment for the fix's position-independent **anchor** byte sequence, accumulating matches across all matching objects.
3. **Verifies** the exact expected original bytes at `match + offset` (and treats an already-patched site as done).
4. **Writes** the patch: `mprotect` the page(s) to `RWX`, copy the replacement bytes, `__builtin___clear_cache` the range, restore `R-X`.

A fix's edits are all located and verified before *any* is written (both-or-nothing). Anything unexpected — pattern not found, more than one match, or wrong bytes — makes that fix log and leave the library untouched.

## Firmware tolerance & safety

- The in-memory anchors were verified present and byte-identical in real 4.38 and 4.45 firmware `libQtGui`/`libQtWebKit`, even though those libraries otherwise diverge, so the same patches hold across the device line. If a future build re-encodes the target, the anchor simply won't match and the fix sits out.
- The hooks (Fixes 1, 2, and 5) bind exact symbols and are `optional`; a rename makes that fix inert.
- The whole mod is inert on 5.x firmware (Qt6 / Chromium — NickelHook doesn't load there).
- Everything is logged to `nickel-type-fix.log` in the config dir, so a single boot shows which fixes engaged. Nothing is written to any device library on disk; a boot without the mod is stock.
