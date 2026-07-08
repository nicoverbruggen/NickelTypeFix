// NickelTypeFix — fixes several text-rendering defects in Kobo's Qt 5.2 WebKit/iType reader
// stack. Each fix is INDEPENDENT and FAIL-SAFE: it engages only if its seam is present on the
// running firmware, and a mismatch on one leaves the others working (nothing is written to disk;
// a boot without the mod is stock). Firmware 4.x only — inert on 5.x (NickelHook won't load).
//
//   1. Glyph "wobble"       — hook FT_Load_Glyph (libkobo), load unhinted   [ntf_no_hinting]
//   2. Vertical (tategaki)  — CSS override on the live page (libnickel)      [ntf_vertfix]
//   3. Justify: koboSpan     — in-memory patch, QTextEngine::justify (libQtGui)   [ntf_justify_kospan]
//   4. Justify: punctuation  — in-memory patch, isInterIdeographExpansionTarget (libQtWebKit) [ntf_justify_punct]
//   5. Reader-font fallback  — re-apply the reader-font CSS per kepub chapter (libnickel)   [ntf_kepub_fontfix]
//
// Cause + fix for each is documented in ABOUT.md. Fixes 1–2 use NickelHook PLT hooks;
// fixes 3–4 patch stripped device libs in memory (locate lib -> position-independent pattern-scan
// -> mprotect + write + flush icache). On first install (no config file yet) this mod also removes
// the superseded standalone mods (NickelHintFix, NickelJustifyFix) so they don't co-load.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE          // dl_iterate_phdr / ElfW (guard: gnu++ dialect may predefine it)
#endif
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <link.h>

#include <QString>
#include <QUrl>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

// ================= shared config =================
static bool ntf_enabled() { return ntf_global_config_bool("ntf_enabled", true); }
// Verbose logging is OFF by default: a healthy boot writes nothing. NTF_DBG lines (status/info) appear
// only when ntf_log is on; NTF_LOG (used for problems: a fix that can't apply, a failed write, a safety
// trip) always writes, so something going wrong is always visible. A problem in the config itself
// (unknown key, malformed line, invalid value) forces verbose logging for the boot — a broken config
// diagnoses itself in the log.
static bool ntf_log()     { return ntf_config_problem_seen() || ntf_global_config_bool("ntf_log", false); }
#define NTF_DBG(...) do { if (ntf_log()) NTF_LOG(__VA_ARGS__); } while (0)

// Built-in default config — written to <config-dir>/config by config.c when it's missing (no
// shipped 'default' file). Kept next to the keys so they stay in sync.
extern "C" const char *const ntf_default_config = R"CFG(#
# NickelTypeFix configuration
#
# Fixes several text-rendering defects in Kobo's reader (firmware 4.x). Each fix is independent:
# it engages only if its seam exists on your firmware, and sits out safely otherwise. Changes
# take effect on reboot. Nothing is written to any device library on disk — a boot without the
# mod is stock.
#

ntf_enabled:1

# Fix 1 - glyph "wobble": load glyphs unhinted so iType stops grid-fitting them to inconsistent
# pixel heights. 0 = stock rendering.
ntf_no_hinting:1
# Comma-separated font families to leave natively hinted (exempt from the above),
# e.g. Georgia, Kobo Nickel
ntf_hinting_allowlist:

# Fix 2 - vertical (tategaki) text: keep vertical CJK books on WebKit's simple path so they
# render correctly with optimizeLegibility on.
ntf_vertfix:1

# Fix 3 - justified text at koboSpan (sentence) boundaries in kepubs (the main justification fix).
ntf_justify_kospan:1
# Fix 4 - justification around punctuation (em/en dashes, ellipses, curly quotes).
ntf_justify_punct:1

# Fix 5 - reader-font fallback: in a kepub book, a chapter's text sometimes renders in the system
# (fallback) font instead of your chosen reading font, because the font was not ready the moment the
# chapter first drew. This re-applies your reading font on each chapter so the text can't stay stuck on
# the fallback. It only affects kepub books, and on a chapter that is already correct it does nothing
# visible. 0 = off.
ntf_kepub_fontfix:1

# Verbose logging to nickel-type-fix.log. Off by default: a healthy boot logs nothing. Problems (a fix
# that can't apply, a failed write, a safety trip) are always logged regardless, and a problem in this
# file (a misspelled setting, an invalid value) turns verbose logging on automatically for that boot.
# 1 = log everything.
ntf_log:0
)CFG";

// The valid config keys, for the parser's unknown-key warning. Kept directly below the default
// config above so the two lists can't drift apart: a key added there must be added here.
extern "C" const char *const ntf_known_keys[] = {
    "ntf_enabled", "ntf_no_hinting", "ntf_hinting_allowlist", "ntf_vertfix",
    "ntf_justify_kospan", "ntf_justify_punct", "ntf_kepub_fontfix", "ntf_log",
    NULL,
};

// ================= FIX 1: hinting "wobble" (libkobo / FT_Load_Glyph) =================
// Minimal FreeType shim — only what we touch (face->family_name for the allow-list).
typedef int FT_Error; typedef signed int FT_Int; typedef signed int FT_Int32; typedef unsigned int FT_UInt;
typedef unsigned short FT_UShort; typedef short FT_Short; typedef long FT_Long; typedef long FT_Pos;
typedef struct FT_BBox_ { FT_Pos xMin, yMin, xMax, yMax; } FT_BBox;
typedef struct FT_Generic_ { void *data, *finalizer; } FT_Generic;
typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;
typedef struct FT_FaceRec_ *FT_Face;
typedef struct FT_FaceRec_ {
    FT_Long num_faces, face_index, face_flags, style_flags, num_glyphs;
    char *family_name, *style_name;
    FT_Int num_fixed_sizes; void *available_sizes; FT_Int num_charmaps; void *charmaps;
    FT_Generic generic; FT_BBox bbox;
    FT_UShort units_per_EM; FT_Short ascender, descender, height, max_advance_width, max_advance_height,
    underline_position, underline_thickness; FT_GlyphSlot glyph;
} FT_FaceRec;

static const FT_Int32 NTF_FT_LOAD_NO_HINTING = 0x2;
static const char *const NTF_LIBKOBO = "/usr/local/Kobo/platforms/libkobo.so";
static FT_Error (*real_FT_Load_Glyph)(FT_Face, FT_UInt, FT_Int32) = nullptr;

// Hinting-scoped safety: if the FT path misbehaves, only the hinting fix passes through — the
// vertical + justify fixes are unaffected. The disabled-by-safety marker persists across boots.
static bool ntf_hint_disabled = false;
static bool ntf_hint_log_dumped = false;

static bool ntf_no_hinting() { return ntf_global_config_bool("ntf_no_hinting", true); }

static bool ntf_font_hinting_allowed(FT_Face face) {
    const char *list = ntf_global_config_get("ntf_hinting_allowlist");
    if (!list || !*list) return false;               // common path never touches the FT shim
    const char *family = (face && face->family_name) ? face->family_name : NULL;
    if (!family || !*family) return false;
    size_t flen = strlen(family);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == flen && !strncasecmp(family, start, flen)) return true;
    }
    return false;
}

static void ntf_hint_write_marker(const char *path, const char *msg) {
    mkdir(NTF_CONFIG_DIR, 0755);
    FILE *f = fopen(path, "w"); if (!f) return;
    if (msg) fprintf(f, "%s\n", msg);
    fclose(f);
}
static void ntf_hint_disable_for_safety(const char *reason) {
    if (ntf_hint_disabled) return;
    ntf_hint_disabled = true;
    NTF_LOG("Safety: the glyph-wobble fix hit a problem and turned itself off for this boot; other fixes keep running. Reason: %s", reason ? reason : "unknown");
    ntf_hint_write_marker(NTF_CONFIG_DIR "/disabled-by-safety", reason);
    if (!ntf_hint_log_dumped) { ntf_hint_log_dumped = true; nh_dump_log(); }
}
static bool ntf_hint_marker_present() {
    static int present = -1;
    if (present == -1) present = (access(NTF_CONFIG_DIR "/disabled-by-safety", F_OK) == 0) ? 1 : 0;
    return present == 1;
}

// ================= FIX 2: vertical (tategaki) text (libnickel) =================
static int  (*ntf_writingDirectionFromString)(const QString &) = nullptr;
static void *(*ntf_cwv_settings)(void *cwv) = nullptr;
static void (*ntf_setUserStyleSheetUrl)(void *settings, const QUrl &url) = nullptr;
static void (*ntf_getUserStyleSheetUrl)(QUrl *sret, void *settings) = nullptr;   // QUrl returned via sret
static void *(*ntf_wv_webView)(void *wv) = nullptr;                              // WebkitView -> its CustomWebView
static void (*real_cwv_setWritingDirection)(void *self, int dir) = nullptr;
static void *(*real_kepubReaderCtor)(void *self, void *pluginState, void *widget) = nullptr;

// The override rule, and the data: URL prefix Nickel itself uses for the user-stylesheet slot
// (StringUtil::encodeAsUrlData formats "data:%1;charset=utf-8;base64,%2", %1 = text/css — verified
// in the firmware disassembly). We speak the exact same format, so a slot written by us and one
// written by Nickel can be told apart, decoded, merged, and unmerged.
static const char *const NTF_VERT_RULE   = "*{text-rendering:auto !important}";
static const char *const NTF_CSS_URL_PFX = "data:text/css;charset=utf-8;base64,";
static int  ntf_wd_vrl = -1, ntf_wd_vlr = -1;
static bool ntf_vertfix_ready = false;
// The set of CustomWebViews currently in a vertical writing mode (per the setWritingDirection hook).
//
// Why this is delicate: the user-stylesheet slot the override lives in is ONE QUrl per view, and it
// is NOT ours alone. Verified in the firmware disassembly: WebkitView::addCssToHtml ==
// setUserStyleSheetUrl(settings(), encodeAsUrlData(css, "text/css")) — every WebkitView-derived view
// (book reader, dictionary, in-app browser, store) stores its own CSS in that same slot, and
// KepubBookReader::addCssToHtml forwards there too, so even the reader's per-chapter font CSS lands
// in it. Two consequences:
//   - never blindly CLEAR a slot: an unconditional clear blanked the dictionary's CSS, rendering the
//     enlarged dictionary's definition text unreadably small (report 53);
//   - never blindly SET one either: replacing the slot on the reader's own view wipes the
//     reading-font CSS, and the next chapter injection wipes our override right back.
// So the override COEXISTS with the slot's owner instead of competing: CSS injections bound for a
// vertical view get the rule appended in flight (_ntf_wv_addCssToHtml), and at a writing-direction
// change the slot is read back (QWebSettings::userStyleSheetUrl) and repaired — set when empty,
// merged into existing CSS, stripped again when the view goes horizontal. The raw pointers here
// carry no lifetime info (a destroyed view's address can be recycled by a later one), so what we do
// to a slot is always decided from the read-back, never from this table alone; the table is also
// flushed on each book open. If the read-back getter is missing on some firmware, we fall back to
// plain per-view set/clear keyed on this table.
#define NTF_VERT_VIEWS_MAX 8
static void *ntf_vert_views[NTF_VERT_VIEWS_MAX];
static bool ntf_vert_view_tracked(void *v) {
    for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (ntf_vert_views[i] == v) return true;
    return false;
}
static void ntf_vert_view_track(void *v, bool on) {
    if (on) {
        if (ntf_vert_view_tracked(v)) return;
        for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (!ntf_vert_views[i]) { ntf_vert_views[i] = v; return; }
        ntf_vert_views[0] = v;   // table full (never expected in practice): reuse slot 0 rather than
                                 // grow — the slot read-back repairs an evicted view's slot later
    } else {
        for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (ntf_vert_views[i] == v) ntf_vert_views[i] = nullptr;
    }
}
static void ntf_vert_views_flush(void) {
    for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) ntf_vert_views[i] = nullptr;
}

static bool ntf_vertfix() { return ntf_global_config_bool("ntf_vertfix", true); }
// Fix 5: reader-font fallback repair (on by default).
static bool ntf_kepub_fontfix() { return ntf_global_config_bool("ntf_kepub_fontfix", true); }

static void ntf_vert_set_url(void *cwv, const QUrl &url) {
    if (!ntf_cwv_settings || !ntf_setUserStyleSheetUrl) return;
    void *settings = ntf_cwv_settings(cwv);
    if (!settings) return;
    ntf_setUserStyleSheetUrl(settings, url);   // an empty QUrl clears the user stylesheet
}

// Encode/decode the slot format shared with Nickel (see NTF_CSS_URL_PFX).
static QUrl ntf_encode_css_url(const QString &css) {
    QByteArray b64 = css.toUtf8().toBase64();
    return QUrl(QString::fromLatin1(NTF_CSS_URL_PFX) + QString::fromLatin1(b64.constData(), b64.size()));
}
static bool ntf_decode_css_url(const QUrl &url, QString *css) {
    QString s = url.toString();
    int comma = s.indexOf(QLatin1Char(','));
    if (comma < 0 || !s.startsWith(QLatin1String("data:text/css"))
        || !s.left(comma).contains(QLatin1String(";base64"))) return false;
    *css = QString::fromUtf8(QByteArray::fromBase64(s.mid(comma + 1).toLatin1()));
    return true;
}

// The pure override (for a slot nothing else uses) as a QUrl — derived from NTF_VERT_RULE through
// the encoder above, so the rule text is the single source of truth: editing it cannot desync the
// set sites from the detect/strip sites. Built once, lazily, on the UI thread.
static const QUrl &ntf_vert_pure_url(void) {
    static const QUrl url = ntf_encode_css_url(QString::fromLatin1(NTF_VERT_RULE));
    return url;
}

// What the view's user-stylesheet slot currently holds. HAS_RULE = our override is in there (alone
// or merged into other CSS); FOREIGN = content without it (decodable or not); UNKNOWN = the
// read-back getter isn't available (or no settings object) and callers fall back to the table.
// On HAS_RULE, and on FOREIGN with *decodable set, *css is the decoded slot content.
enum ntf_vert_slot_t { NTF_SLOT_UNKNOWN, NTF_SLOT_EMPTY, NTF_SLOT_HAS_RULE, NTF_SLOT_FOREIGN };
static ntf_vert_slot_t ntf_vert_slot(void *cwv, QString *css, bool *decodable) {
    *decodable = false;
    if (!ntf_getUserStyleSheetUrl || !ntf_cwv_settings) return NTF_SLOT_UNKNOWN;
    void *settings = ntf_cwv_settings(cwv);
    if (!settings) return NTF_SLOT_UNKNOWN;
    QUrl url;
    ntf_getUserStyleSheetUrl(&url, settings);
    if (url.isEmpty()) return NTF_SLOT_EMPTY;
    if (url == ntf_vert_pure_url()) {   // exact pure override; skip the decode
        *decodable = true;
        *css = QString::fromLatin1(NTF_VERT_RULE);
        return NTF_SLOT_HAS_RULE;
    }
    if (!ntf_decode_css_url(url, css)) return NTF_SLOT_FOREIGN;
    *decodable = true;
    return css->contains(QString::fromLatin1(NTF_VERT_RULE)) ? NTF_SLOT_HAS_RULE : NTF_SLOT_FOREIGN;
}

// ================= FIX 5: reader-font fallback repair (libnickel) =================
// In a kepub book the reading font is applied as an injected `* { font-family:'<font>' !important; }`
// rule (KepubBookReader::pageStyleCss -> addCssToHtml) resolved against a QFontDatabase application
// font. If the font isn't ready the instant a chapter first draws, that chapter renders its text in a
// substitute (the system fallback) and stays that way on page turns (only a font change or a reopen
// clears it). This fix re-applies the reader-font rule once per chapter: pageStyleCss rebuilds the rule
// and KepubBookReader::addCssToHtml removes the old frame rule (QWebFrame::removeCSSRule) and re-sets
// the page's user stylesheet through the base WebkitView::addCssToHtml (verified in the firmware
// disassembly), so WebKit re-cascades and re-resolves the font in place. It is the same re-inject the reader itself runs
// on a font size/family change (applyStyling), minus the repaginate, so it doesn't move the reading
// position; on an already-correct chapter it renders the identical font, so it is invisible.
//
// Arming: WebkitView::addCssToHtml fires when a chapter injects its font CSS (a per-chapter, font-
// agnostic signal). We arm there and consume on the next WebkitView::setCurrentPage (after the chapter
// has drawn). Our own re-inject also calls addCssToHtml, so ntf_in_fixonturn suppresses re-arming.
static void (*real_wv_addCssToHtml)(void *self, QString *css) = nullptr;     // WebkitView::addCssToHtml (arm)
static void (*real_wv_setCurrentPage)(void *self, int page) = nullptr;       // WebkitView::setCurrentPage (consume)
static void (*ntf_pageStyleCss)(QString *sret, void *reader, bool arg) = nullptr;   // KepubBookReader::pageStyleCss (QString sret)
static void (*ntf_kbr_addCssToHtml)(void *reader, QString *css) = nullptr;          // KepubBookReader::addCssToHtml (QString by ptr)
static void *ntf_kepub_reader = nullptr;            // captured in the KepubBookReader ctor
static bool ntf_in_fixonturn = false;               // re-entrancy guard around the re-inject
static bool ntf_chapter_needs_fix = false;          // armed by a chapter's font-CSS injection, consumed on the next setCurrentPage
static bool ntf_fontfix_logged = false;             // the friendly "fix is active" note, once per book

// Re-apply the reader-font CSS into the live document. Caller must hold ntf_in_fixonturn and have
// verified the syms. Logs one friendly note per book; per-chapter detail only under verbose logging.
static void ntf_do_reinject(int page) {
    QString css;
    ntf_pageStyleCss(&css, ntf_kepub_reader, false);   // false = do not force the fixed-layout body block
    (void)page;
    if (!ntf_fontfix_logged) {
        ntf_fontfix_logged = true;
        NTF_DBG("Reader-font fix: re-applying your reading font on each chapter of this book, so the text can't get stuck showing the fallback (system) font.");
    }
    ntf_kbr_addCssToHtml(ntf_kepub_reader, &css);
}

// ================= FIX 3+4: justification (in-memory byte patches) =================
// TIMING: these edits run from ntf_init, which NickelHook calls from its library __constructor
// as Nickel dlopen()s this plugin at startup — long before any book is opened. The patched
// functions (QTextEngine::justify, isInterIdeographExpansionTarget) only execute during page
// layout of an opened book, so no thread is mid-execution in them when we write. That is what
// makes the non-atomic 2-byte writes safe; keep the patch phase in init if init ordering changes.
// koboSpan fix — libQtGui, QTextEngine::justify, two sites (both required, both-or-nothing).
static const unsigned char KOS_A_ANCHOR[] = { 0x15,0xF8,0x01,0x3C, 0xD8,0x06, 0x40,0xF1,0x9E,0x80, 0x04,0xE0 };
static const unsigned char KOS_A_ORIG[]   = { 0x40,0xF1,0x9E,0x80 };   // bpl.w -> b.w (skip trim loop)
static const unsigned char KOS_A_REPL[]   = { 0x00,0xF0,0x9E,0xB8 };
static const unsigned char KOS_B_ANCHOR[] = { 0x2C,0x46, 0x51,0xE7, 0x63,0x1E, 0x3B,0x61, 0xDC,0xD0 };
static const unsigned char KOS_B_ORIG[]   = { 0x63,0x1E };             // subs r3,r4,#1 -> movs r3,r4
static const unsigned char KOS_B_REPL[]   = { 0x23,0x00 };
// punctuation fix — libQtWebKit, isInterIdeographExpansionTarget, one site (anchor+0x18).
static const unsigned char PUN_ANCHOR[] = {
    0xa0,0xf5,0x00,0x52, 0xa2,0xf1,0x10,0x03, 0x01,0x2b, 0x8c,0xbf,
    0x01,0x23, 0x00,0x23, 0x6f,0x2a, 0x88,0xbf, 0x00,0x23, 0x0b,0xb1,
};
static const unsigned char PUN_ORIG[] = { 0x18,0x46 };   // mov r0,r3 -> movs r0,#0
static const unsigned char PUN_REPL[] = { 0x00,0x20 };

struct ntf_patch_t {
    const char *label; const char *incl, *excl;
    const unsigned char *anchor; int anchor_len; int off;
    const unsigned char *orig, *repl; int plen;
};
#define NTF_MAXP 2
struct ntf_fix_t { const char *name, *cfg_key; bool cfg_default; struct ntf_patch_t patch[NTF_MAXP]; int n; };

static const struct ntf_fix_t NTF_JUSTIFY_FIXES[] = {
    { "koboSpan (QTextEngine::justify)", "ntf_justify_kospan", true, {
        { "justify:skip-trim", "Gui", NULL,    KOS_A_ANCHOR, (int)sizeof(KOS_A_ANCHOR), 6, KOS_A_ORIG, KOS_A_REPL, (int)sizeof(KOS_A_ORIG) },
        { "justify:range-len", "Gui", NULL,    KOS_B_ANCHOR, (int)sizeof(KOS_B_ANCHOR), 4, KOS_B_ORIG, KOS_B_REPL, (int)sizeof(KOS_B_ORIG) },
    }, 2 },
    { "punctuation (isInterIdeographExpansionTarget)", "ntf_justify_punct", true, {
        { "expansion-target", "WebKit", "Widgets", PUN_ANCHOR, (int)sizeof(PUN_ANCHOR), (int)sizeof(PUN_ANCHOR), PUN_ORIG, PUN_REPL, (int)sizeof(PUN_ORIG) },
        { 0 },
    }, 1 },
};

static const unsigned char *ntf_scan(const unsigned char *hay, size_t haylen,
                                     const unsigned char *needle, size_t nlen, int *count) {
    const unsigned char *first = NULL; int c = 0;
    if (haylen >= nlen)
        for (size_t i = 0; i + nlen <= haylen; i++)
            if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) { if (!first) first = hay + i; c++; }
    *count = c; return first;
}
struct ntf_find { const char *incl, *excl; const unsigned char *needle; int nlen; int total; const unsigned char *match; };
static int ntf_find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; struct ntf_find *f = (struct ntf_find *)data;
    const char *name = info->dlpi_name;
    if (!name || !*name) return 0;
    if (!strstr(name, f->incl)) return 0;
    if (f->excl && strstr(name, f->excl)) return 0;   // e.g. exclude the small WebKitWidgets wrapper
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        const unsigned char *seg = (const unsigned char *)(info->dlpi_addr + ph->p_vaddr);
        int c = 0;
        const unsigned char *m = ntf_scan(seg, (size_t)ph->p_memsz, f->needle, (size_t)f->nlen, &c);
        if (c > 0) { if (!f->match) f->match = m; f->total += c; }
    }
    return 0;
}
static void ntf_forceload(void) {
    static const char *cands[] = {
        "libQt5WebKit.so.5", "libQtWebKit.so.4", "libQt5Gui.so.5", "libQtGui.so.4",
        "/usr/local/Qt-5.2.1-arm/lib/libQt5WebKit.so.5", "/usr/local/Qt-5.2.1-arm/lib/libQt5Gui.so.5",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        void *h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (!h) h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        (void)h;
    }
}
static bool ntf_write(const unsigned char *site, const unsigned char *repl, int len) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    uintptr_t addr = (uintptr_t)site, page = addr & ~(uintptr_t)(pg - 1);
    size_t span = (size_t)((addr + (unsigned)len) - page);
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        NTF_LOG("mprotect(RWX) failed at %p: %s", (void *)page, strerror(errno)); return false;
    }
    for (int i = 0; i < len; i++) ((unsigned char *)site)[i] = repl[i];
    __builtin___clear_cache((char *)site, (char *)site + len);
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0)
        NTF_LOG("warning: mprotect(R-X) restore failed at %p: %s", (void *)page, strerror(errno));
    return true;
}
// Locate + verify every edit in a fix; write them only if all located and verified (both-or-nothing).
static void ntf_apply_justify_fix(const struct ntf_fix_t *fx) {
    if (!ntf_global_config_bool(fx->cfg_key, fx->cfg_default)) { NTF_DBG("Justification fix (%s) is turned off in config; skipping.", fx->name); return; }
    const unsigned char *sites[NTF_MAXP]; bool already[NTF_MAXP];
    for (int i = 0; i < fx->n; i++) {
        const struct ntf_patch_t *p = &fx->patch[i];
        struct ntf_find f = { p->incl, p->excl, p->anchor, p->anchor_len, 0, NULL };
        dl_iterate_phdr(ntf_find_cb, &f);
        NTF_DBG("  [%s] %s: matches=%d", fx->name, p->label, f.total);
        if (f.total == 0) { NTF_LOG("Justification fix (%s) could not attach on this firmware and is sitting out (other fixes are unaffected).", fx->name); return; }
        if (f.total > 1)  { NTF_LOG("Justification fix (%s) sat out to be safe (its target was not unique on this firmware).", fx->name); return; }
        const unsigned char *site = f.match + p->off; already[i] = false;
        if (memcmp(site, p->repl, (size_t)p->plen) == 0) { NTF_DBG("  [%s] %s already patched", fx->name, p->label); already[i] = true; }
        else if (memcmp(site, p->orig, (size_t)p->plen) != 0) { NTF_LOG("Justification fix (%s) sat out to be safe (unexpected code at its target on this firmware).", fx->name); return; }
        sites[i] = site;
    }
    int wrote = 0;
    for (int i = 0; i < fx->n; i++) {
        if (already[i]) continue;
        if (ntf_write(sites[i], fx->patch[i].repl, fx->patch[i].plen)) { wrote++; continue; }
        // write failed mid-fix: restore any site we already patched so the fix stays both-or-nothing
        NTF_LOG("Justification fix (%s) could not be applied and was rolled back cleanly (no change made).", fx->name);
        for (int j = i - 1; j >= 0; j--)
            if (!already[j]) ntf_write(sites[j], fx->patch[j].orig, fx->patch[j].plen);
        return;
    }
    NTF_DBG("Justification fix (%s) is active.", fx->name);
}

// ================= startup: remove the superseded standalone mods =================
static void ntf_rmtree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(child, &st) && S_ISDIR(st.st_mode)) ntf_rmtree(child); else unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}
static void ntf_remove_superseded(void) {
    // While our init runs during startup, a co-loaded NickelHook mod has failsafe-renamed itself
    // to <name>.failsafe (it renames back a few seconds later), so unlink that name too or a
    // live superseded mod escapes the one-shot cleanup.
    static const char *old_so[] = {
        "/usr/local/Kobo/imageformats/libnickelhintfix.so",
        "/usr/local/Kobo/imageformats/libnickelhintfix.so.failsafe",
        "/usr/local/Kobo/imageformats/libnickeljustifyfix.so",
        "/usr/local/Kobo/imageformats/libnickeljustifyfix.so.failsafe",
    };
    for (size_t i = 0; i < sizeof(old_so) / sizeof(old_so[0]); i++) {
        if (access(old_so[i], F_OK) != 0) { NTF_DBG("superseded plugin %s not present (%s)", old_so[i], strerror(errno)); continue; }
        if (unlink(old_so[i]) == 0) NTF_DBG("Removed an older mod this one replaces: %s", old_so[i]);
        else NTF_LOG("Note: could not remove an older mod this one replaces (%s): %s", old_so[i], strerror(errno));
    }
    static const char *old_dir[] = {
        "/mnt/onboard/.adds/nickelhintfix", "/mnt/onboard/.adds/nickeljustifyfix",
    };
    for (size_t i = 0; i < sizeof(old_dir) / sizeof(old_dir[0]); i++) {
        if (access(old_dir[i], F_OK) != 0) { NTF_DBG("superseded config dir %s not present (%s)", old_dir[i], strerror(errno)); continue; }
        ntf_rmtree(old_dir[i]);   // best-effort recursive delete; verify the result below
        if (access(old_dir[i], F_OK) == 0) NTF_LOG("Note: could not fully remove an older mod's settings folder: %s", old_dir[i]);
        else NTF_DBG("Removed an older mod's settings folder: %s", old_dir[i]);
    }
}

// ================= init =================
static int ntf_init() {
    // First-install detection: the config file is the one first-boot artifact we create ourselves
    // (the doc and uninstall marker ship inside KoboRoot.tgz, so they exist from the very first
    // boot). Check before priming the config, which writes the missing file.
    bool first_install = (access(NTF_CONFIG_DIR "/config", F_OK) != 0);
    ntf_global_config_get("");                      // prime config while single-threaded
    if (first_install)
        ntf_remove_superseded();                    // stop the old standalone mods co-loading
    if (!ntf_enabled()) { NTF_DBG("NickelTypeFix is turned off in its config (ntf_enabled:0); nothing was changed."); return 0; }
    NTF_DBG("NickelTypeFix started. Fixes turned on -> glyph wobble: %s, vertical text: %s, justification: %s, reader font: %s.",
        ntf_no_hinting() ? "yes" : "no",
        ntf_vertfix() ? "yes" : "no",
        (ntf_global_config_bool("ntf_justify_kospan", true) || ntf_global_config_bool("ntf_justify_punct", true)) ? "yes" : "no",
        ntf_kepub_fontfix() ? "yes" : "no");

    // FIX 2 (vertical): learn the vertical-writing-mode enum values from Nickel itself.
    NTF_DBG("vertical syms cwvSetDir=%p cwvSettings=%p setUserCss=%p getUserCss=%p wvWebView=%p kepubCtor=%p wdFromString=%p",
        (void *)real_cwv_setWritingDirection, (void *)ntf_cwv_settings, (void *)ntf_setUserStyleSheetUrl,
        (void *)ntf_getUserStyleSheetUrl, (void *)ntf_wv_webView, (void *)real_kepubReaderCtor,
        (void *)ntf_writingDirectionFromString);
    if (ntf_writingDirectionFromString) {
        ntf_wd_vrl = ntf_writingDirectionFromString(QStringLiteral("vertical-rl"));
        ntf_wd_vlr = ntf_writingDirectionFromString(QStringLiteral("vertical-lr"));
        ntf_vertfix_ready = true;
        NTF_DBG("vertical-rl=%d vertical-lr=%d", ntf_wd_vrl, ntf_wd_vlr);
    } else {
        NTF_LOG("Note: the vertical-text fix could not attach on this firmware, so it is sitting out (other fixes are unaffected).");
    }
    if (ntf_hint_marker_present()) NTF_LOG("Note: the glyph-wobble fix is off this boot (it disabled itself earlier for safety); other fixes still run.");

    // FIX 3+4 (justify): pattern-scan + patch the loaded libs in memory.
    ntf_forceload();
    for (size_t i = 0; i < sizeof(NTF_JUSTIFY_FIXES) / sizeof(NTF_JUSTIFY_FIXES[0]); i++)
        ntf_apply_justify_fix(&NTF_JUSTIFY_FIXES[i]);
    return 0;
}

// ================= uninstall / wiring =================
static bool ntf_del(const char *p) { return (access(p, F_OK) && errno == ENOENT) ? true : nh_delete_file(p); }
static bool ntf_uninstall() {
    NTF_DBG("Uninstalling NickelTypeFix: removing its files. The in-memory fixes revert on the next boot.");
    bool ok = true;
    ok = ntf_del(NTF_CONFIG_DIR "/doc") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/config") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/nickel-type-fix.log") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/disabled-by-safety") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/uninstall") && ok;
    if (access(NTF_CONFIG_DIR, F_OK) == 0) ok = nh_delete_dir(NTF_CONFIG_DIR) && ok;
    return ok;
}

static struct nh_info NickelTypeFixInfo = {
    .name            = "NickelTypeFix",
    .desc            = "Fix Kobo reader text rendering: hinting wobble, vertical text, and justification.",
    .uninstall_flag  = NTF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NTF_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};
static struct nh_hook NickelTypeFixHooks[] = {
    // FIX 1 — now OPTIONAL so a missing FT symbol only sits out hinting (independence).
    { .sym = "FT_Load_Glyph", .sym_new = "_ntf_FT_Load_Glyph", .lib = NTF_LIBKOBO,
      .out = nh_symoutptr(real_FT_Load_Glyph), .desc = "load glyphs unhinted", .optional = true },
    // FIX 2 — optional.
    { .sym = "_ZN13CustomWebView19setWritingDirectionE16WritingDirection", .sym_new = "_ntf_cwv_setWritingDirection",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_cwv_setWritingDirection), .desc = "inject text-rendering:auto for vertical books", .optional = true },
    // FIX 5 — reader-font fallback repair: the ctor resets per-book state; arm on the per-chapter
    // font-CSS injection, re-inject on the next page-set. All optional (a missing symbol just sits
    // the fix out).
    { .sym = "_ZN15KepubBookReaderC1EP11PluginStateP7QWidget", .sym_new = "_ntf_kepubReaderCtor",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kepubReaderCtor), .desc = "fix 5: reset per-book state", .optional = true },
    { .sym = "_ZN10WebkitView12addCssToHtmlE7QString", .sym_new = "_ntf_wv_addCssToHtml",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_addCssToHtml), .desc = "arm reader-font re-apply", .optional = true },
    { .sym = "_ZN10WebkitView14setCurrentPageEi", .sym_new = "_ntf_wv_setCurrentPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_setCurrentPage), .desc = "re-apply reader font per chapter", .optional = true },
    {0},
};
static struct nh_dlsym NickelTypeFixDlsym[] = {
    { .name = "_Z26writingDirectionFromStringRK7QString", .out = nh_symoutptr(ntf_writingDirectionFromString), .desc = "derive vertical enum ints", .optional = true },
    { .name = "_ZNK13CustomWebView8settingsEv", .out = nh_symoutptr(ntf_cwv_settings), .desc = "reach the page's QWebSettings", .optional = true },
    { .name = "_ZN12QWebSettings20setUserStyleSheetUrlERK4QUrl", .out = nh_symoutptr(ntf_setUserStyleSheetUrl), .desc = "set/clear the user stylesheet", .optional = true },
    { .name = "_ZNK12QWebSettings17userStyleSheetUrlEv", .out = nh_symoutptr(ntf_getUserStyleSheetUrl), .desc = "read the slot back before touching it", .optional = true },
    { .name = "_ZNK10WebkitView7webViewEv", .out = nh_symoutptr(ntf_wv_webView), .desc = "map a WebkitView to its CustomWebView", .optional = true },
    { .name = "_ZN15KepubBookReader12pageStyleCssEb", .out = nh_symoutptr(ntf_pageStyleCss), .desc = "fix 5: rebuild reader-font CSS", .optional = true },
    { .name = "_ZN15KepubBookReader12addCssToHtmlE7QString", .out = nh_symoutptr(ntf_kbr_addCssToHtml), .desc = "fix 5: re-inject reader-font CSS", .optional = true },
    {0},
};

NickelHook(
    .init      = &ntf_init,
    .info      = &NickelTypeFixInfo,
    .hook      = NickelTypeFixHooks,
    .dlsym     = NickelTypeFixDlsym,
    .uninstall = &ntf_uninstall,
)

// ================= hook bodies =================
// FIX 1 — hinting. Independent: if disabled-by-safety, only hinting passes through.
extern "C" __attribute__((visibility("default")))
FT_Error _ntf_FT_Load_Glyph(FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags) {
    if (!real_FT_Load_Glyph) { ntf_hint_disable_for_safety("real FT_Load_Glyph was NULL"); return 1; }
    if (ntf_hint_disabled || ntf_hint_marker_present())
        return real_FT_Load_Glyph(face, glyph_index, load_flags);
    // Orthogonal to iType's CSM stem-weighting (Font Weight) — that's set before the load.
    FT_Int32 eff = load_flags;
    if (ntf_enabled() && ntf_no_hinting() && !ntf_font_hinting_allowed(face))
        eff |= NTF_FT_LOAD_NO_HINTING;
    return real_FT_Load_Glyph(face, glyph_index, eff);
}

// FIX 5 + FIX 2 — per-book reset: capture the reader pointer for the Fix 5 re-inject, re-arm the
// chapter font fix, and drop Fix 2's vertical-view tracking (any tracked view is from a previous
// book; a still-live vertical view is re-tracked at its next setWritingDirection, and dropping a
// destroyed view's pointer here keeps a recycled address from inheriting its vertical status).
extern "C" __attribute__((visibility("default")))
void *_ntf_kepubReaderCtor(void *self, void *pluginState, void *widget) {
    ntf_kepub_reader = self;       // reader ptr for the Fix 5 re-inject (this = KepubBookReader)
    ntf_chapter_needs_fix = false; // Fix 5: re-armed by each chapter's font-CSS injection
    ntf_fontfix_logged = false;    // let Fix 5 log its one friendly note again for this book
    ntf_vert_views_flush();        // Fix 2: stale per-view state must not survive into a new book
    if (real_kepubReaderCtor) return real_kepubReaderCtor(self, pluginState, widget);
    return self;
}
extern "C" __attribute__((visibility("default")))
void _ntf_cwv_setWritingDirection(void *self, int dir) {
    bool vert = ntf_vertfix_ready && (dir == ntf_wd_vrl || dir == ntf_wd_vlr);
    if (ntf_enabled() && ntf_vertfix() && ntf_vertfix_ready) {
        // Repair the slot from what it ACTUALLY holds (see ntf_vert_views): set only an empty slot,
        // merge into (never replace) existing CSS, strip only our own rule. The table is never, by
        // itself, a reason to clear — a stale entry from a destroyed view whose address got recycled
        // must not blank the new view's own CSS (rpt 53). Once a view is tracked as vertical, the
        // injection hook (_ntf_wv_addCssToHtml) keeps the rule present across later slot rewrites.
        bool tracked = ntf_vert_view_tracked(self);
        QString css;
        bool decodable = false;
        ntf_vert_slot_t slot = ntf_vert_slot(self, &css, &decodable);
        NTF_DBG("setWritingDirection view=%p dir=%d (vert=%d) tracked=%d slot=%d", self, dir, vert ? 1 : 0, tracked ? 1 : 0, (int)slot);
        ntf_vert_view_track(self, vert);
        if (vert) {
            if (slot == NTF_SLOT_EMPTY) {
                ntf_vert_set_url(self, ntf_vert_pure_url());
            } else if (slot == NTF_SLOT_FOREIGN && decodable) {
                // The view's own CSS is already in the slot (e.g. the reader injected its font CSS
                // before the writing mode was known): merge our rule in, keeping theirs intact.
                NTF_DBG("vertical view %p: merging the text-rendering override into the slot's existing CSS", self);
                ntf_vert_set_url(self, ntf_encode_css_url(css + QLatin1Char('\n') + QString::fromLatin1(NTF_VERT_RULE)));
            } else if (slot == NTF_SLOT_FOREIGN) {
                NTF_DBG("vertical view %p: slot holds CSS in an unrecognized format; leaving it untouched", self);
            } else if (slot == NTF_SLOT_UNKNOWN && !tracked) {
                ntf_vert_set_url(self, ntf_vert_pure_url());   // no read-back: old set/clear behavior
            }
        } else {
            if (slot == NTF_SLOT_HAS_RULE) {
                // Nickel transiently applies dir=0 (horizontal) on the reader view during every
                // chapter transition, before the new chapter's writing mode is parsed (observed on
                // device), so in a vertical book this strip runs once per chapter and the vertical
                // branch re-merges moments later. Both keep the slot's other CSS intact.
                css.remove(QString::fromLatin1(NTF_VERT_RULE));
                css = css.trimmed();
                ntf_vert_set_url(self, css.isEmpty() ? QUrl() : ntf_encode_css_url(css));
            } else if (slot == NTF_SLOT_UNKNOWN && tracked) {
                ntf_vert_set_url(self, QUrl());   // no read-back: old set/clear behavior
            }
        }
    }
    if (real_cwv_setWritingDirection) real_cwv_setWritingDirection(self, dir);
}

// FIX 5 — arm the per-chapter re-inject. WebkitView::addCssToHtml is called when a chapter injects its
// font CSS (once per chapter load; not on plain page turns), which is our per-chapter, font-agnostic
// "a fresh chapter drew" signal. Our own re-inject also calls this (via KepubBookReader::addCssToHtml),
// so ntf_in_fixonturn suppresses re-arming to avoid a loop. `css` is passed by hidden reference; we
// only read state, never mutate it.
extern "C" __attribute__((visibility("default")))
void _ntf_wv_addCssToHtml(void *self, QString *css) {
    if (ntf_enabled() && ntf_kepub_fontfix() && !ntf_in_fixonturn)
        ntf_chapter_needs_fix = true;
    // FIX 2: this call REPLACES the view's whole user-stylesheet slot (it re-encodes `css` as a
    // data: URL and hands it to setUserStyleSheetUrl — see ntf_vert_views), which would wipe a
    // previously-set vertical override. If the injection is bound for a view we know is vertical,
    // carry the override inside the injected CSS so both survive in the one slot. `css` is this
    // call's own by-value copy (a caller-owned temporary per the ARM C++ ABI), so appending here
    // only affects this call.
    if (ntf_enabled() && ntf_vertfix() && ntf_vertfix_ready && ntf_wv_webView && css) {
        void *cwv = ntf_wv_webView(self);
        bool tracked = cwv && ntf_vert_view_tracked(cwv);
        bool append = tracked && !css->contains(QString::fromLatin1(NTF_VERT_RULE));
        NTF_DBG("addCssToHtml wv=%p cwv=%p tracked=%d append=%d", self, cwv, tracked ? 1 : 0, append ? 1 : 0);
        if (append) css->append(QLatin1Char('\n')).append(QString::fromLatin1(NTF_VERT_RULE));
    }
    if (real_wv_addCssToHtml) real_wv_addCssToHtml(self, css);
}

// FIX 5 — consume: on the first setCurrentPage after a chapter drew (armed above), re-apply the
// reader-font CSS into the live document. If the chapter rendered its text in a substitute because the
// font was not ready in time, this re-resolves it in place; if the chapter is already correct the
// re-apply renders the identical font and is invisible.
extern "C" __attribute__((visibility("default")))
void _ntf_wv_setCurrentPage(void *self, int page) {
    if (real_wv_setCurrentPage) real_wv_setCurrentPage(self, page);
    if (ntf_enabled() && ntf_kepub_fontfix() && ntf_chapter_needs_fix && !ntf_in_fixonturn
        && ntf_pageStyleCss && ntf_kbr_addCssToHtml && ntf_kepub_reader) {
        ntf_chapter_needs_fix = false;
        ntf_in_fixonturn = true;
        ntf_do_reinject(page);
        ntf_in_fixonturn = false;
    }
}
