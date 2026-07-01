# How NickelTypeFix works

NickelTypeFix corrects four text-rendering defects in Kobo's old **Qt 5.2 / QtWebKit / Monotype
iType** reader stack (firmware 4.x). Each fix is **independent** — it engages only if its seam is
present on the running firmware and sits out safely otherwise, so a mismatch on one never affects
the others. Fixes 1–2 use NickelHook PLT hooks; fixes 3–4 patch stripped device libraries **in
memory** at startup (locate the loaded lib → scan for a position-independent byte pattern → verify
→ `mprotect` + write + flush the icache). Nothing is written to any device library on disk, so a
boot without the mod is stock.

Each section below states the bug and the minimal, reversible change the mod makes to fix it. The
mod modifies nothing on the device's storage and redistributes no Kobo, Qt, or Monotype code —
every fix is a small interoperability bugfix applied in memory at boot that disappears the moment
the mod is removed.

---

## Fix 1 — Glyph "wobble" (hinting)   ·   `ntf_no_hinting`

**The problem.** With certain fonts, individual letters drift up or down by a pixel relative to
the baseline, giving a visibly uneven line. The same fonts render cleanly on desktops and in
KOReader.

**The cause.** Kobo's rasterizer, Monotype **iType**, is registered as a hinting-capable driver,
so when Nickel loads a glyph *with hinting requested* (its default), iType grid-fits the outline.
For a glyph that carries **no per-glyph hinting instructions**, iType falls back to its own
automatic grid-fitting and snaps the glyph's top to a whole pixel row. That snap is
sub-pixel-position-sensitive, so the *same letter* lands on a different integer height depending
on where it falls in the line — the wobble. (It is not the font's `gasp`/`fpgm`/`prep` tables:
the render path never consults them for uninstructed glyphs.)

**The fix.** Hook `FT_Load_Glyph` (imported by Kobo's `libkobo.so` platform plugin) and OR in
`FT_LOAD_NO_HINTING` (`0x208` → `0x20a`) before the real call. That sets iType's "skip grid-fit"
flag, so it emits the raw scaled outline — every instance of a glyph then has identical geometry.
Hinting buys little at ~300 DPI, so this is low-cost; `ntf_hinting_allowlist` exempts any family
you want to keep natively hinted. The Font Weight / sharpness (CSM) controls are a separate stage
and are unaffected.


---

## Fix 2 — Vertical (tategaki) CJK text   ·   `ntf_vertfix`

**The problem.** With the WebKit **`optimizeLegibility`** text-rendering flag on (
`webkitTextRendering=optimizeLegibility`), vertical Japanese/Chinese books render with the
long-vowel mark `ー`, brackets `「」`, and punctuation `、。` horizontal or mislaid.

**The cause.** `optimizeLegibility` forces WebKit's **complex text path** (QTextLayout-based),
which has no vertical-writing-mode support. WebKit's **simple path** renders tategaki correctly —
the only reason the book lands on the broken path is the injected `optimizeLegibility`.

**The fix.** The obvious seams don't work (`pageStyleCss`/`setWritingDirection` are virtual, so
not PLT-hookable; `getWebkitTextRendering` is read before writing-mode is known). Instead, once a
view's writing mode is applied (`CustomWebView::setWritingDirection`, PLT-hookable) and it's
vertical, push a tiny **user stylesheet** — `*{text-rendering:auto !important}` — onto that page's
`QWebSettings`. A user-origin `!important` rule outranks the author's `optimizeLegibility`, so
WebKit re-renders on the simple path. For non-vertical views the stylesheet is cleared, so
horizontal books keep `optimizeLegibility` untouched. Enum values are derived at runtime from
Nickel's own `writingDirectionFromString` (no hardcoded magic numbers).


---

## Fix 3 — Justified text at koboSpan boundaries (the main justify fix)   ·   `ntf_justify_kospan`

**The problem.** In a **kepub** with Justification + `optimizeLegibility` on, justified lines that
cross a sentence boundary get a starved space at the boundary while the line's other gaps
over-stretch — an uneven line. It happens in plain text (no special punctuation).

**The cause.** kepubify wraps every sentence in its own `<span class="koboSpan">`. On the complex
path WebKit draws each span as a **separate justification run**, and Qt's `QTextEngine::justify`
distributes expansion only over `[from … from+length−1]` — the run's trailing space (the koboSpan
boundary) lives in `si.trailingSpaces`, *outside* `si.length`, so it never receives the expansion
WebKit already budgeted for it. The boundary space stays natural width; the span's internal
spaces soak up the slack.

**The fix.** A surgical two-byte-pair in-memory patch to `QTextEngine::justify` in `libQtGui`,
making its range include the trailing space so the budget lands where it belongs:

| site | change | bytes |
|---|---|---|
| A | skip the trailing-trim loop (`bpl.w`→`b.w`) | `40 F1 9E 80` → `00 F0 9E B8` |
| B | justify range = length, not length−1 (`subs r3,r4,#1`→`movs r3,r4`) | `63 1E` → `23 00` |

Both are required (each alone is a no-op), so they're applied both-or-nothing. Validated to be
byte-identical to a correct reference render, with **zero regression** on normal / single-span /
CJK / simple-path text. Works for all existing kepubs (no re-conversion), keeps ligatures.


---

## Fix 4 — Justification around punctuation   ·   `ntf_justify_punct`

**The problem.** Justified text can space unevenly around em/en dashes, ellipses, and curly
quotes.

**The cause.** Kobo widened `WebCore::Font::isInterIdeographExpansionTarget` to treat General
Punctuation **U+2000–U+206F** as justification-expansion targets. On the simple path this injects
phantom expansion opportunities around that punctuation.

**The fix.** A one-byte-pair in-memory patch to that function in `libQtWebKit`, making the
U+2000–U+206F range return false (`mov r0,r3` → `movs r0,#0`, i.e. `18 46` → `00 20`, at the
prologue anchor + 0x18). Every other range (real CJK, symbols, fullwidth) is untouched. This is
the secondary justify fix; the koboSpan fix (3) is the one that addresses the common visible bug.


---

## Firmware tolerance & safety

Fixes 3–4 scan for **position-independent instruction patterns**, not absolute offsets, so they
survive the library moving across firmware builds — verified that the exact anchors are present
and byte-identical in real 4.38 and 4.45 firmware `libQtGui`/`libQtWebKit` even though the libs
otherwise diverge. If a pattern isn't found (or is ambiguous, or the bytes differ),
that fix logs and leaves the library untouched. Fixes 1–2 bind exact symbols and are `optional`,
so a firmware rename just makes that fix inert. The whole mod is inert on 5.x firmware (Qt6 /
Chromium — NickelHook doesn't load there).

Everything is logged to `nickel-type-fix.log` in the config dir, so a single boot shows which
fixes engaged.
