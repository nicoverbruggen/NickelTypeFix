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
//   6. Letter-spacing spaces — in-memory patch, QTextEngine::shapeText (libQtGui)      [ntf_letterspace_spaces]
//   7. Capital spacing (cpsp) — strip the cpsp feature per font at load, any font (libnickel/QFontDatabase)  [ntf_cpsp_fix]
//
// Cause + fix for each is documented in ABOUT.md. Fixes 1, 2, and 5 use NickelHook PLT hooks;
// fixes 3–4 patch stripped device libs in memory (locate lib -> position-independent pattern-scan
// -> bounded permission change -> write -> verify -> flush icache -> restore permissions). On first
// install (no config file yet) this mod also removes the superseded standalone mods (NickelHintFix,
// NickelJustifyFix) so they don't co-load.

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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>

#include <QString>
#include <QUrl>
#include <QByteArray>
#include <QFontDatabase>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

// ================= shared config =================
// Return the master switch.  Every hook checks this before changing behavior,
// so `ntf_enabled:0` is the closest equivalent to removing the plugin without
// uninstalling it.
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

# Fix 7 - capital spacing (cpsp): some fonts carry an OpenType 'cpsp' (Capital Spacing) feature meant
# only for all-caps text. Kobo's reader applies it to ordinary body text too, pushing every capital
# away from the next letter (a loose gap after a capital, e.g. the D in "Docks"). This removes cpsp
# from each font as it loads, for any font. Kerning and every other feature are left untouched. 0 = off.
ntf_cpsp_fix:1

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
    "ntf_justify_kospan", "ntf_justify_punct", "ntf_kepub_fontfix",
    "ntf_letterspace_spaces", "ntf_cpsp_fix", "ntf_log",
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
// Both flags are read and written from whatever threads rasterize glyphs, so
// all access goes through relaxed atomics (no ordering is needed — each flag
// is an independent latch).
static bool ntf_hint_disabled = false;
static bool ntf_hint_log_dumped = false;

// Read the Fix 1 switch separately from the master switch so the user can
// leave the other rendering fixes enabled while restoring stock hinting.
static bool ntf_no_hinting() { return ntf_global_config_bool("ntf_no_hinting", true); }

// Match the allowlist against FreeType's actual family_name, not a filename or
// display label.  Matching is case-insensitive and tolerates comma-separated
// whitespace so the setting remains easy to edit by hand.
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

// The marker is the persistent circuit breaker for Fix 1.  It is deliberately
// fail-closed: if the filesystem cannot be inspected, we must not re-enable a
// fix that already reported a runtime safety problem.
enum ntf_hint_marker_state_t {
    NTF_HINT_MARKER_UNKNOWN = -1,
    NTF_HINT_MARKER_ABSENT = 0,
    NTF_HINT_MARKER_PRESENT = 1,
    NTF_HINT_MARKER_UNSAFE = 2,
};
// Stored as int so the cache can be read/written with relaxed atomics: it is
// primed in ntf_init but also reachable from the FT hook's render threads.
static int ntf_hint_marker_cached = NTF_HINT_MARKER_UNKNOWN;

// Read the persistent circuit-breaker state once per boot. Caching avoids a
// filesystem call on every glyph while still making all later decisions use
// the same conservative result. Two threads racing the first read both probe
// the filesystem and store an equally conservative result, so the race is
// benign by construction.
static ntf_hint_marker_state_t ntf_hint_marker_state(void) {
    int cached = __atomic_load_n(&ntf_hint_marker_cached, __ATOMIC_RELAXED);
    if (cached != NTF_HINT_MARKER_UNKNOWN) return (ntf_hint_marker_state_t)cached;

    int state;
    if (access(NTF_CONFIG_DIR "/disabled-by-safety", F_OK) == 0) {
        state = NTF_HINT_MARKER_PRESENT;
    } else if (errno == ENOENT) {
        // ENOENT is the only clean "not disabled" result.  An absent parent
        // directory also lands here, which is the normal first-install state.
        state = NTF_HINT_MARKER_ABSENT;
    } else {
        // EACCES, EIO, and similar errors mean we cannot establish that the
        // circuit breaker is clear.  Keep hinting disabled for this boot.
        state = NTF_HINT_MARKER_UNSAFE;
        NTF_LOG("Safety: could not read the glyph-wobble marker; keeping the hinting fix disabled. Reason: %s", strerror(errno));
    }
    __atomic_store_n(&ntf_hint_marker_cached, state, __ATOMIC_RELAXED);
    return (ntf_hint_marker_state_t)state;
}

// Persist the reason for a hinting safety shutdown without ever truncating the
// existing marker first. The return value distinguishes a durable trip from a
// boot-local shutdown so callers can report the remaining limitation.
static bool ntf_hint_write_marker(const char *path, const char *msg) {
    if (mkdir(NTF_CONFIG_DIR, 0755) != 0 && errno != EEXIST) {
        NTF_LOG("Safety: could not create the glyph-wobble marker directory: %s", strerror(errno));
        return false;
    }

    // Write and flush a uniquely named sibling first, then rename it into
    // place.  O_EXCL|O_NOFOLLOW prevents a pre-existing symlink from turning
    // the safety write into an overwrite outside the config directory.
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        NTF_LOG("Safety: glyph-wobble marker path is too long");
        return false;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        NTF_LOG("Safety: could not open the temporary glyph-wobble marker: %s", strerror(errno));
        return false;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp);
        NTF_LOG("Safety: could not wrap the temporary glyph-wobble marker: %s", strerror(saved_errno));
        return false;
    }

    bool ok = true;
    if (msg && fprintf(f, "%s\n", msg) < 0) ok = false;
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        NTF_LOG("Safety: could not flush the glyph-wobble marker: %s", strerror(errno));
        unlink(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        NTF_LOG("Safety: could not install the glyph-wobble marker: %s", strerror(errno));
        unlink(tmp);
        return false;
    }

    // fsync(file) makes the marker contents durable; fsync(directory) makes
    // the rename durable as well.  If the filesystem cannot guarantee that,
    // report failure rather than claiming the safety trip will persist.
    int dir_fd = open(NTF_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0 || fsync(dir_fd) != 0) {
        int saved_errno = errno;
        if (dir_fd >= 0) close(dir_fd);
        NTF_LOG("Safety: could not sync the glyph-wobble marker directory: %s", strerror(saved_errno));
        return false;
    }
    close(dir_fd);
    __atomic_store_n(&ntf_hint_marker_cached, NTF_HINT_MARKER_PRESENT, __ATOMIC_RELAXED);
    return true;
}

// Disable only Fix 1 after an unexpected FreeType seam failure. The other
// fixes remain available, and the marker prevents the same hinting path from
// being retried on the next boot.
static void ntf_hint_disable_for_safety(const char *reason) {
    // Atomic exchange doubles as the "first caller wins" gate when several
    // glyph threads trip the same problem at once.
    if (__atomic_exchange_n(&ntf_hint_disabled, true, __ATOMIC_RELAXED)) return;
    NTF_LOG("Safety: the glyph-wobble fix hit a problem and turned itself off for this boot; other fixes keep running. Reason: %s", reason ? reason : "unknown");
    if (!ntf_hint_write_marker(NTF_CONFIG_DIR "/disabled-by-safety", reason))
        NTF_LOG("Safety: the glyph-wobble fix is disabled only for this boot because its persistent marker could not be saved.");
    if (!__atomic_exchange_n(&ntf_hint_log_dumped, true, __ATOMIC_RELAXED)) nh_dump_log();
}

// ================= FIX 2: vertical (tategaki) text (libnickel) =================
static int  (*ntf_writingDirectionFromString)(const QString &) = nullptr;
static void *(*ntf_cwv_settings)(void *cwv) = nullptr;
static void (*ntf_setUserStyleSheetUrl)(void *settings, const QUrl &url) = nullptr;
static void (*ntf_getUserStyleSheetUrl)(QUrl *sret, void *settings) = nullptr;   // QUrl returned via sret
static void *(*ntf_wv_webView)(void *wv) = nullptr;                              // WebkitView -> its CustomWebView
static void (*real_cwv_setWritingDirection)(void *self, int dir) = nullptr;
// C1/C2 constructor entry points have a void ABI.  Keeping the function type
// exact matters even though callers normally ignore the value in r0.
static void (*real_kepubReaderCtor)(void *self, void *pluginState, void *widget) = nullptr;

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
// The table stores only CustomWebView identities. CSS contents are read back
// from each view because object addresses can be recycled after destruction.
static bool ntf_vert_view_tracked(void *v) {
    for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (ntf_vert_views[i] == v) return true;
    return false;
}
// Add or remove one view. A full table leaves the new view untracked instead
// of evicting a live view whose override would then stop being maintained.
static void ntf_vert_view_track(void *v, bool on) {
    if (on) {
        if (ntf_vert_view_tracked(v)) return;
        for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (!ntf_vert_views[i]) { ntf_vert_views[i] = v; return; }
        // Do not evict a live view: doing so would leave its override installed
        // but stop maintaining it on the next stylesheet rewrite.  A ninth
        // simultaneous vertical view is rare; leaving the new one untracked is
        // safer than corrupting the state of an existing one.
        NTF_LOG("vertical view table is full; leaving view %p untracked", v);
    } else {
        for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) if (ntf_vert_views[i] == v) ntf_vert_views[i] = nullptr;
    }
}
// A new book invalidates all remembered view identities, preventing a recycled
// address from inheriting the previous book's vertical-writing state.
static void ntf_vert_views_flush(void) {
    for (int i = 0; i < NTF_VERT_VIEWS_MAX; i++) ntf_vert_views[i] = nullptr;
}

// Feature accessors keep configuration policy readable at call sites and make
// the intended default explicit next to each implementation.
static bool ntf_vertfix() { return ntf_global_config_bool("ntf_vertfix", true); }
// Fix 5: reader-font fallback repair (on by default).
static bool ntf_kepub_fontfix() { return ntf_global_config_bool("ntf_kepub_fontfix", true); }

// Update the one user-stylesheet URL slot owned by this CustomWebView. Callers
// must classify the slot first so this helper does not erase CSS owned by
// Nickel's reader, dictionary, browser, or store views.
static void ntf_vert_set_url(void *cwv, const QUrl &url) {
    if (!ntf_cwv_settings || !ntf_setUserStyleSheetUrl) return;
    void *settings = ntf_cwv_settings(cwv);
    if (!settings) return;
    ntf_setUserStyleSheetUrl(settings, url);   // an empty QUrl clears the user stylesheet
}

// Encode/decode the slot format shared with Nickel (see NTF_CSS_URL_PFX).
// Using Nickel's existing data URL format lets us recognize and merge our rule
// without introducing a second storage protocol.
static QUrl ntf_encode_css_url(const QString &css) {
    QByteArray b64 = css.toUtf8().toBase64();
    return QUrl(QString::fromLatin1(NTF_CSS_URL_PFX) + QString::fromLatin1(b64.constData(), b64.size()));
}
// Decode only canonical CSS data URLs. Malformed or foreign content is left
// untouched rather than being replaced with a partial decode.
static bool ntf_decode_css_url(const QUrl &url, QString *css) {
    QString s = url.toString();
    int comma = s.indexOf(QLatin1Char(','));
    if (comma < 0 || !s.startsWith(QLatin1String("data:text/css"))
        || !s.left(comma).contains(QLatin1String(";base64"))) return false;
    QByteArray encoded = s.mid(comma + 1).toLatin1();
    QByteArray decoded = QByteArray::fromBase64(encoded);
    // Nickel emits canonical base64 without whitespace.  Re-encoding catches
    // malformed input that Qt's permissive decoder would otherwise accept and
    // turn into an empty or altered stylesheet.
    if (!encoded.isEmpty() && decoded.toBase64() != encoded) return false;
    *css = QString::fromUtf8(decoded);
    return true;
}

// The pure override (for a slot nothing else uses) as a QUrl — derived from NTF_VERT_RULE through
// the encoder above, so the rule text is the single source of truth: editing it cannot desync the
// set sites from the detect/strip sites. Built once, lazily, on the UI thread.
// This URL is used for exact ownership detection when the slot contains only
// NickelTypeFix's rule.
static const QUrl &ntf_vert_pure_url(void) {
    static const QUrl url = ntf_encode_css_url(QString::fromLatin1(NTF_VERT_RULE));
    return url;
}

// What the view's user-stylesheet slot currently holds. HAS_RULE = our override is in there (alone
// or merged into other CSS); FOREIGN = content without it (decodable or not); UNKNOWN = the
// read-back getter isn't available (or no settings object) and callers fall back to the table.
// On HAS_RULE, and on FOREIGN with *decodable set, *css is the decoded slot content.
enum ntf_vert_slot_t { NTF_SLOT_UNKNOWN, NTF_SLOT_EMPTY, NTF_SLOT_HAS_RULE, NTF_SLOT_FOREIGN };
// Classify the current slot and return its CSS when it is decodable. The
// caller uses this result to merge or remove only its own rule.
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
static void (*real_kepubReaderDtor)(void *self) = nullptr;
// Fix 5 uses a raw pointer because NickelHook exposes C++ objects as opaque
// addresses.  KepubBookReader is multiple-inherited: on the validated 4.45
// firmware, WebkitView is the +24-byte subobject, so its hook's `self` is not
// the complete-object pointer received by the constructor.  The destructor
// clears both identities before the object can disappear.  If a future ABI
// changes that offset, Fix 5 simply fails to arm rather than calling a wrong
// object.
//
// Future-firmware fallback (implemented in ntf_learn_reader_view): when the
// validated `_ZThn24_` thunk is absent, the offset is learned from the live
// objects instead of assumed.  Note the originally sketched "exactly one
// shared adjustment between the KepubBookReader and WebkitView thunk sets"
// heuristic does NOT work: on the validated firmware KepubBookReader emits
// destructor thunks at N = 8, 24, 216, 236, and 520 (several non-primary
// bases), and the set shared with WebkitView is {8, 24} — not unique.
#define NTF_KEPUB_WEBKIT_OFFSET 24
static void *ntf_kepub_reader = nullptr;
static void *ntf_kepub_reader_view = nullptr;
static void (*ntf_kepubReaderWebkitDtorThunk)(void *self) = nullptr;
static void *ntf_chapter_view = nullptr;            // view that armed the pending chapter repair
static bool ntf_in_fixonturn = false;               // re-entrancy guard around the re-inject
static bool ntf_chapter_needs_fix = false;          // armed by a reader CSS injection, consumed by that view
static bool ntf_fontfix_logged = false;             // the friendly "fix is active" note, once per book

// GUI-thread guard for the Qt-side hooks. All Fix 2/5 state above is plain
// (unsynchronized) data whose check-then-use sequences are only race-free
// because Nickel calls every hooked QWidget-side method on its one GUI thread.
// That is true today; this guard turns the assumption into a checked one. The
// first Qt-side hook invocation claims the thread; a call arriving on any
// other thread makes our logic sit out (the real function still runs), so a
// future firmware that moved one of these calls to a worker thread degrades to
// a logged no-op instead of racing on a reader pointer mid-destruction.
static uintptr_t ntf_qt_thread = 0;   // 0 = unclaimed (pthread_self() is a TCB address on glibc, never 0)
static bool ntf_qt_thread_warned = false;
static bool ntf_on_qt_thread(void) {
    uintptr_t self = (uintptr_t)pthread_self(), expected = 0;
    if (__atomic_compare_exchange_n(&ntf_qt_thread, &expected, self, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
        || expected == self)
        return true;
    if (!__atomic_exchange_n(&ntf_qt_thread_warned, true, __ATOMIC_RELAXED))
        NTF_LOG("Note: a hooked GUI call arrived on an unexpected thread; the affected fix is sitting out (other fixes are unaffected).");
    return false;
}

// Re-apply the reader-font CSS into the live document. Caller must hold ntf_in_fixonturn and have
// verified the syms. Logs one friendly note per book; per-chapter detail only under verbose logging.
// `reader` is the complete KepubBookReader object; the WebkitView hook's `self`
// is only the +24 base subobject on the supported firmware.
static void ntf_do_reinject(void *reader, int page) {
    QString css;
    ntf_pageStyleCss(&css, reader, false);   // false = do not force the fixed-layout body block
    (void)page;
    if (!ntf_fontfix_logged) {
        ntf_fontfix_logged = true;
        NTF_DBG("Reader-font fix: re-applying your reading font on each chapter of this book, so the text can't get stuck showing the fallback (system) font.");
    }
    ntf_kbr_addCssToHtml(reader, &css);
}

// Future-firmware fallback: called from the WebkitView CSS-injection hook only
// when the resolve-time `_ZThn24_` thunk was absent (so the +24 layout is not
// proven for this firmware) and no view has been learned for the live reader
// yet. `self` is the WebkitView receiving addCssToHtml; it is the reader's own
// view iff it is a base subobject of the live KepubBookReader. Prove that from
// the C++ ABI instead of assuming a layout: the candidate offset must have a
// matching this-adjusting destructor thunk in libnickel, AND that exact thunk
// must appear in the vtable the candidate subobject actually points at — which
// ties the offset to this object's dynamic type, not to a lookalike heap
// neighbour (a different complete object's vtable never contains another
// class's `_ZThn` destructor thunks). Every failure path leaves Fix 5 inert
// for the book; this can delay the fix on an unknown firmware but can never
// aim a call at the wrong object. GUI-thread only (callers hold the guard).
static bool ntf_learn_reader_view(void *self) {
    if (!ntf_kepub_reader || !self) return false;
    uintptr_t base = (uintptr_t)ntf_kepub_reader, cand = (uintptr_t)self;
    if (cand <= base) return false;                    // offset 0 (primary base) has no thunk to prove it
    uintptr_t off = cand - base;
    if (off > 1024 || off % 4 != 0) return false;      // sane single-object layouts only
    char sym[64];
    int n = snprintf(sym, sizeof(sym), "_ZThn%u_N15KepubBookReaderD1Ev", (unsigned)off);
    if (n < 0 || (size_t)n >= sizeof(sym)) return false;
    static void *libnickel = dlopen("libnickel.so.1.0.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!libnickel) return false;
    void *thunk = dlsym(libnickel, sym);
    if (!thunk) return false;
    // The destructor sits within the first few virtual slots for every class in
    // this hierarchy; 12 bounds the scan while staying inside the vtable.
    void **vtable = *(void ***)self;
    if (!vtable) return false;
    for (int i = 0; i < 12; i++) {
        if (vtable[i] == thunk) {
            ntf_kepub_reader_view = self;
            NTF_DBG("Reader-font fix: discovered the reader's view at offset +%u on this firmware.", (unsigned)off);
            return true;
        }
    }
    return false;
}

// NOTE: "letter-spacing on spaces" (ntf_letterspace_spaces) is implemented as an in-memory byte patch
// alongside the justification fixes below (see LSP_ANCHOR / NTF_JUSTIFY_FIXES), not a hook. Root cause:
// QTextEngine::shapeText tracks every glyph, then a word/space loop subtracts letterSpacing back off
// each space and the letter before it (Qt's non-spec "no tracking around whitespace"). The patch NOPs
// the two subtracts so spaces and pre-space letters keep their tracking, matching browsers/CSS Text 3;
// wordSpacing is untouched, and it is a no-op when letterSpacing==0.

// ================= FIX 3+4: justification (in-memory byte patches) =================
// TIMING: these edits run from ntf_init, which NickelHook calls from its library __constructor
// as Nickel dlopen()s this plugin at startup — long before any book is opened. Nickel already has
// several threads at this point, so the page remains executable during the short write window:
// removing execute permission could fault an unrelated function which shares that page. Safety
// instead comes from patching before the two target layout functions are used, then immediately
// verifying the bytes, flushing the instruction cache, and restoring the original permissions.
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
// letter-spacing on spaces fix — libQtGui, QTextEngine::shapeText word/space loop, two sites at one
// anchor (both required, both-or-nothing). shapeText adds letterSpacing to every glyph, then this loop
// subtracts it back off each space and the letter before it (Qt's non-spec "no tracking around
// whitespace"), then adds wordSpacing to the space. NOP the two subtracts so spaces and pre-space
// letters keep their tracking (spec-correct, matches browsers); wordSpacing is untouched. Each subtract
// is `advances -= letterSpacing`, a no-op when letterSpacing==0, so non-tracked text is unaffected.
static const unsigned char LSP_ANCHOR[] = {
    0x43,0x68, 0x18,0xBF, 0x05,0x68, 0xCA,0xEB,0x03,0x03, 0x18,0xBF,
    0xCA,0xEB,0x05,0x05, 0x43,0x60, 0x18,0xBF, 0x05,0x60,
};
static const unsigned char LSP_A_ORIG[] = { 0xCA,0xEB,0x03,0x03 };   // rsb  r3,sl,r3  (space -= ls)
static const unsigned char LSP_B_ORIG[] = { 0xCA,0xEB,0x05,0x05 };   // rsbne r5,sl,r5 (pre-space letter -= ls)
static const unsigned char LSP_REPL[]   = { 0xAF,0xF3,0x00,0x80 };   // nop.w

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
    { "letter-spacing on spaces (QTextEngine::shapeText)", "ntf_letterspace_spaces", true, {
        { "letterspace:space",     "Gui", NULL, LSP_ANCHOR, (int)sizeof(LSP_ANCHOR), 6,  LSP_A_ORIG, LSP_REPL, (int)sizeof(LSP_A_ORIG) },
        { "letterspace:preletter", "Gui", NULL, LSP_ANCHOR, (int)sizeof(LSP_ANCHOR), 12, LSP_B_ORIG, LSP_REPL, (int)sizeof(LSP_B_ORIG) },
    }, 2 },
};

static const unsigned char *ntf_scan(const unsigned char *hay, size_t haylen,
                                     const unsigned char *needle, size_t nlen, int *count) {
    // Return the first match for patching, but count every match so callers can
    // reject ambiguous firmware layouts instead of choosing arbitrarily.
    const unsigned char *first = NULL; int c = 0;
    if (haylen >= nlen)
        for (size_t i = 0; i + nlen <= haylen; i++)
            if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) { if (!first) first = hay + i; c++; }
    *count = c; return first;
}
struct ntf_find {
    const char *incl, *excl;
    const unsigned char *needle;
    int nlen;
    int total;
    const unsigned char *match;
    const unsigned char *segment;
    size_t segment_len;
    int segment_prot;
};
static int ntf_segment_prot(unsigned flags) {
    // Translate ELF PT_LOAD flags into the protection mask to restore after a
    // patch. A writable executable segment is rejected later as unsafe.
    int prot = 0;
    if (flags & PF_R) prot |= PROT_READ;
    if (flags & PF_W) prot |= PROT_WRITE;
    if (flags & PF_X) prot |= PROT_EXEC;
    return prot;
}
static int ntf_find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    // Scan only executable PT_LOAD segments of matching libraries. The callback
    // records the segment metadata alongside the first match so the patch range
    // can be checked before any pointer arithmetic or write occurs.
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
        if (c > 0) {
            if (!f->match) {
                f->match = m;
                f->segment = seg;
                f->segment_len = (size_t)ph->p_memsz;
                f->segment_prot = ntf_segment_prot(ph->p_flags);
            }
            f->total += c;
        }
    }
    return 0;
}
static void ntf_forceload(void) {
    // Force-load the Qt libraries whose stripped code may contain the optional
    // justification targets. RTLD_NOLOAD avoids unnecessary duplicate loads.
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

// Verify that the entire edit, not just its anchor, belongs to the executable
// segment that was scanned.  This keeps a future firmware mismatch from making
// memcmp() or the write walk past the mapped code range.
static bool ntf_patch_site(const struct ntf_find *f, const struct ntf_patch_t *p,
                           const unsigned char **site) {
    if (!f->match || !f->segment || f->segment_len == 0 || p->off < 0 || p->plen <= 0)
        return false;
    size_t match_offset = (size_t)(f->match - f->segment);
    if (match_offset > f->segment_len || (size_t)p->off > f->segment_len - match_offset)
        return false;
    size_t site_offset = match_offset + (size_t)p->off;
    if ((size_t)p->plen > f->segment_len - site_offset)
        return false;
    *site = f->match + p->off;
    return true;
}

// Apply one byte edit while temporarily adding write permission, then restore
// the segment's original ELF-derived permissions. `changed` tells the caller to
// include this site in rollback even when permission restoration fails after
// the bytes were written.
static bool ntf_write(const unsigned char *site, const unsigned char *repl, int len,
                      int restore_prot, bool *changed) {
    *changed = false;
    uintptr_t addr = (uintptr_t)site;
    // Thumb-2 guarantees only halfword alignment, even for 32-bit instructions,
    // so a 4-byte site may legitimately sit at addr % 4 == 2 (on the validated
    // 4.6.2 firmware the koboSpan anchor itself starts a 32-bit instruction at
    // such an address). Requiring word alignment here would make the fix sit
    // out on firmware builds where the function shifts by a halfword.
    if ((len != 2 && len != 4) || addr % 2 != 0
        || !(restore_prot & PROT_EXEC) || (restore_prot & PROT_WRITE)) {
        NTF_LOG("refusing an invalid executable patch request at %p", (const void *)site);
        return false;
    }

    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    uintptr_t last = addr + (uintptr_t)len - 1;
    if (last < addr) {
        NTF_LOG("executable patch address overflow at %p", (const void *)site);
        return false;
    }
    uintptr_t page = addr - (addr % (uintptr_t)pg);
    uintptr_t last_page = last - (last % (uintptr_t)pg);
    // A halfword-aligned 4-byte edit can straddle a page boundary
    // (addr % pagesize == pagesize - 2), so the permission change must cover
    // every page the edit touches. ntf_patch_site has already verified the
    // whole edit lies inside the one executable PT_LOAD segment that was
    // scanned, so a second page belongs to the same mapping and gets the same
    // restore_prot — this never alters an adjacent mapping with other flags.
    size_t span = (size_t)(last_page - page) + (size_t)pg;

    // Nickel has other threads by the time this plugin is loaded. Keep the
    // page executable so an unrelated function sharing it cannot fault while
    // the target bytes are changed. The target layout functions themselves do
    // not run until a book is opened, after this init phase has completed.
    if (mprotect((void *)page, span, restore_prot | PROT_WRITE) != 0) {
        NTF_LOG("mprotect(write enable) failed at %p: %s", (void *)page, strerror(errno));
        return false;
    }
    // Use a single-copy atomic store whenever natural alignment allows it, so
    // another thread cannot fetch a partially written instruction if it
    // reaches the target unexpectedly. A 4-byte site at addr % 4 == 2 cannot
    // be stored as one uint32_t (misaligned atomics are UB, and the store
    // would not be single-copy atomic anyway), so it is split into its two
    // naturally aligned halfwords. That split is free of instruction tearing
    // ONLY because of the TIMING invariant above: these writes happen in
    // ntf_init, before the patched layout functions can run on any thread.
    // Do not reuse this path for a patch applied after startup.
    if (len == 2) {
        uint16_t value;
        memcpy(&value, repl, sizeof(value));
        __atomic_store_n((uint16_t *)site, value, __ATOMIC_RELEASE);
    } else if (addr % 4 == 0) {
        uint32_t value;
        memcpy(&value, repl, sizeof(value));
        __atomic_store_n((uint32_t *)site, value, __ATOMIC_RELEASE);
    } else {
        uint16_t lo, hi;
        memcpy(&lo, repl, sizeof(lo));
        memcpy(&hi, repl + 2, sizeof(hi));
        __atomic_store_n((uint16_t *)site, lo, __ATOMIC_RELEASE);
        __atomic_store_n((uint16_t *)(site + 2), hi, __ATOMIC_RELEASE);
    }
    *changed = true;
    __builtin___clear_cache((char *)site, (char *)site + len);

    bool bytes_ok = memcmp(site, repl, (size_t)len) == 0;
    if (!bytes_ok)
        NTF_LOG("executable patch verification failed at %p", (const void *)site);

    bool restored = mprotect((void *)page, span, restore_prot) == 0;
    if (!restored)
        NTF_LOG("mprotect(permission restore) failed at %p: %s", (void *)page, strerror(errno));
    return bytes_ok && restored;
}
// Both KOS anchors overlap their own edit bytes, so a site that already
// carries the replacement — patched by the superseded standalone
// NickelJustifyFix running first, or by another instance of this plugin in the
// same process — no longer matches the primary scan and would be misreported
// as "could not attach" even though the intended bytes are in place. Rescan
// with the replacement substituted into the anchor; on a unique match, fill
// `f` so the caller's normal "already patched" path takes over. (The PUN edit
// lies outside its anchor, so its already-patched state is caught by the
// primary scan and this helper declines.)
static bool ntf_scan_already_patched(const struct ntf_patch_t *p, struct ntf_find *f) {
    unsigned char patched[32];
    if (p->anchor_len <= 0 || (size_t)p->anchor_len > sizeof(patched)) return false;
    if (p->off < 0 || p->plen <= 0 || p->off + p->plen > p->anchor_len) return false;
    memcpy(patched, p->anchor, (size_t)p->anchor_len);
    memcpy(patched + p->off, p->repl, (size_t)p->plen);
    struct ntf_find pf = { p->incl, p->excl, patched, p->anchor_len, 0, NULL, NULL, 0, 0 };
    dl_iterate_phdr(ntf_find_cb, &pf);
    if (pf.total != 1) return false;   // an ambiguous patched site is as unsafe as an ambiguous original
    pf.needle = NULL;                  // `patched` dies with this frame; never expose it
    pf.nlen = 0;
    *f = pf;
    return true;
}

// Locate + verify every edit in a fix; write them only if all located and verified (both-or-nothing).
static bool ntf_apply_justify_fix(const struct ntf_fix_t *fx) {
    if (!ntf_global_config_bool(fx->cfg_key, fx->cfg_default)) { NTF_DBG("Justification fix (%s) is turned off in config; skipping.", fx->name); return true; }
    const unsigned char *sites[NTF_MAXP]; int restore_prot[NTF_MAXP]; bool already[NTF_MAXP];
    for (int i = 0; i < fx->n; i++) {
        const struct ntf_patch_t *p = &fx->patch[i];
        struct ntf_find f = { p->incl, p->excl, p->anchor, p->anchor_len, 0, NULL, NULL, 0, 0 };
        dl_iterate_phdr(ntf_find_cb, &f);
        NTF_DBG("  [%s] %s: matches=%d", fx->name, p->label, f.total);
        if (f.total == 0 && ntf_scan_already_patched(p, &f))
            NTF_DBG("  [%s] %s: found in already-patched form (applied earlier by this plugin or a superseded mod)", fx->name, p->label);
        if (f.total == 0) { NTF_LOG("Justification fix (%s) could not attach on this firmware and is sitting out (other fixes are unaffected).", fx->name); return true; }
        if (f.total > 1)  { NTF_LOG("Justification fix (%s) sat out to be safe (its target was not unique on this firmware).", fx->name); return true; }
        const unsigned char *site = NULL;
        if (!ntf_patch_site(&f, p, &site)) {
            NTF_LOG("Justification fix (%s) sat out to be safe (its patch range is outside the executable segment).", fx->name);
            return true;
        }
        sites[i] = site;
        restore_prot[i] = f.segment_prot;
        already[i] = false;
        if (memcmp(site, p->repl, (size_t)p->plen) == 0) { NTF_DBG("  [%s] %s already patched", fx->name, p->label); already[i] = true; }
        else if (memcmp(site, p->orig, (size_t)p->plen) != 0) { NTF_LOG("Justification fix (%s) sat out to be safe (unexpected code at its target on this firmware).", fx->name); return true; }
    }
    for (int i = 0; i < fx->n; i++) {
        if (already[i]) continue;
        bool changed = false;
        if (ntf_write(sites[i], fx->patch[i].repl, fx->patch[i].plen, restore_prot[i], &changed)) continue;

        // A write can fail after changing bytes (for example, when restoring
        // page permissions fails), so roll back the current site as well as all
        // earlier sites.  Never claim a clean rollback unless every restore was
        // verified and its permissions were restored.
        bool rollback_ok = true;
        NTF_LOG("Justification fix (%s) failed while applying; attempting a complete rollback.", fx->name);
        for (int j = i; j >= 0; j--) {
            if (already[j] || (j == i && !changed)) continue;
            bool rollback_changed = false;
            if (!ntf_write(sites[j], fx->patch[j].orig, fx->patch[j].plen,
                           restore_prot[j], &rollback_changed))
                rollback_ok = false;
        }
        if (rollback_ok)
            NTF_LOG("Justification fix (%s) was rolled back cleanly; it is disabled for this boot.", fx->name);
        else {
            NTF_LOG("CRITICAL: justification fix (%s) could not verify its rollback; process memory is unsafe and Nickel must stop while NickelHook's boot failsafe is still armed.", fx->name);
            return false;
        }
        return true;
    }
    NTF_DBG("Justification fix (%s) is active.", fx->name);
    return true;
}

// ================= startup: remove the superseded standalone mods =================
// Open every path component without following symlinks.  The cleanup only
// targets hard-coded mod directories, but those directories live on
// user-writable storage, so path-string recursion would still be racy.
// The returned descriptor is the trusted starting point for all later openat
// and unlinkat operations.
static int ntf_open_dir_path(const char *path) {
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char component[256];
        size_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n + 1 >= sizeof(component)) { close(fd); errno = ENAMETOOLONG; return -1; }
            component[n] = p[n];
            n++;
        }
        component[n] = '\0';
        while (p[n] == '/') n++;
        int next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { close(fd); return -1; }
        close(fd);
        fd = next;
        p += n;
    }
    return fd;
}

// Remove one directory entry using descriptor-relative operations. Directories
// are opened with O_NOFOLLOW; symlinks and files are deleted as leaves, never
// traversed. A race can at worst make removal fail, not redirect recursion.
static bool ntf_rmtree_at(int parent_fd, const char *name) {
    int dir_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0) {
        if (errno == ENOENT) return true;
        // A non-directory is removed as a leaf; symlinks are never opened as
        // directories because of O_NOFOLLOW and are removed, not traversed.
        if (errno == ENOTDIR || errno == ELOOP)
            return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;
        NTF_LOG("could not open superseded path %s: %s", name, strerror(errno));
        return false;
    }

    DIR *dir = fdopendir(dir_fd);
    if (!dir) { close(dir_fd); return false; }
    int current_fd = dirfd(dir);
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(current_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!ntf_rmtree_at(current_fd, entry->d_name)) ok = false;
        } else if (unlinkat(current_fd, entry->d_name, 0) != 0 && errno != ENOENT) {
            ok = false;
        }
    }
    if (closedir(dir) != 0) ok = false;
    if (!ok) return false;
    return unlinkat(parent_fd, name, AT_REMOVEDIR) == 0 || errno == ENOENT;
}

static bool ntf_rmtree(const char *path) {
    // Open the parent through the no-symlink path walker, then remove the
    // target by descriptor-relative names so a concurrent rename cannot redirect
    // recursion outside the intended mod directory.
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1]) return false;
    char parent[1024];
    size_t parent_len = (size_t)(slash - path);
    if (parent_len == 0) parent_len = 1; // path was directly below /
    if (parent_len >= sizeof(parent)) return false;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';

    int parent_fd = ntf_open_dir_path(parent);
    if (parent_fd < 0) {
        if (errno == ENOENT) return true;
        NTF_LOG("could not open superseded directory parent %s: %s", parent, strerror(errno));
        return false;
    }
    bool ok = ntf_rmtree_at(parent_fd, slash + 1);
    close(parent_fd);
    return ok;
}
static void ntf_remove_superseded(void) {
    // First-install-only migration: remove the two older standalone plugins so
    // they cannot co-load and patch the same Nickel process. Every target is
    // hard-coded and the recursive deletion above never follows symlinks.
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
    // NickelHook calls this during plugin loading, before a book is opened. It
    // resolves optional hooks, validates runtime-dependent values, applies the
    // in-memory patches, and returns an error only when process memory cannot be
    // proven safe after a failed rollback.
    // First-install detection: the config file is the one first-boot artifact we create ourselves
    // (the doc and uninstall marker ship inside KoboRoot.tgz, so they exist from the very first
    // boot). Check before priming the config, which writes the missing file.
    bool first_install = (access(NTF_CONFIG_DIR "/config", F_OK) != 0);
    ntf_global_config_get("");                      // prime config before any hook can read it
    if (first_install)
        ntf_remove_superseded();                    // stop the old standalone mods co-loading
    if (!ntf_enabled()) { NTF_DBG("NickelTypeFix is turned off in its config (ntf_enabled:0); nothing was changed."); return 0; }
    NTF_DBG("NickelTypeFix started. Fixes turned on -> glyph wobble: %s, vertical text: %s, justification: %s, reader font: %s.",
        ntf_no_hinting() ? "yes" : "no",
        ntf_vertfix() ? "yes" : "no",
        (ntf_global_config_bool("ntf_justify_kospan", true) || ntf_global_config_bool("ntf_justify_punct", true)) ? "yes" : "no",
        ntf_kepub_fontfix() ? "yes" : "no");

    // FIX 2 (vertical): learn the vertical-writing-mode enum values from Nickel itself.
    NTF_DBG("vertical/reader syms cwvSetDir=%p cwvSettings=%p setUserCss=%p getUserCss=%p wvWebView=%p kepubCtor=%p kepubDtor=%p kepubWebkitThunk=%p wdFromString=%p",
        (void *)real_cwv_setWritingDirection, (void *)ntf_cwv_settings, (void *)ntf_setUserStyleSheetUrl,
        (void *)ntf_getUserStyleSheetUrl, (void *)ntf_wv_webView, (void *)real_kepubReaderCtor,
        (void *)real_kepubReaderDtor, (void *)ntf_kepubReaderWebkitDtorThunk,
        (void *)ntf_writingDirectionFromString);
    if (ntf_writingDirectionFromString) {
        ntf_wd_vrl = ntf_writingDirectionFromString(QStringLiteral("vertical-rl"));
        ntf_wd_vlr = ntf_writingDirectionFromString(QStringLiteral("vertical-lr"));
        // A failed lookup or a broken firmware parser must not make every
        // direction look vertical.  We only accept two distinct non-negative
        // values; the actual enum numbers remain firmware-defined.
        if (ntf_wd_vrl >= 0 && ntf_wd_vlr >= 0 && ntf_wd_vrl != ntf_wd_vlr) {
            ntf_vertfix_ready = true;
            NTF_DBG("vertical-rl=%d vertical-lr=%d", ntf_wd_vrl, ntf_wd_vlr);
        } else {
            NTF_LOG("Note: vertical-writing enum values were invalid (%d, %d); the vertical-text fix is sitting out.", ntf_wd_vrl, ntf_wd_vlr);
        }
    } else {
        NTF_LOG("Note: the vertical-text fix could not attach on this firmware, so it is sitting out (other fixes are unaffected).");
    }
    ntf_hint_marker_state_t marker = ntf_hint_marker_state();
    if (marker == NTF_HINT_MARKER_PRESENT) {
        NTF_LOG("Note: the glyph-wobble fix is off this boot (it disabled itself earlier for safety); other fixes still run.");
    } else if (marker == NTF_HINT_MARKER_UNSAFE) {
        // Do not let an unreadable marker turn a previous safety trip back on.
        __atomic_store_n(&ntf_hint_disabled, true, __ATOMIC_RELAXED);
        NTF_LOG("Note: the glyph-wobble fix is off this boot because its safety state could not be verified; other fixes still run.");
    }

    // FIX 3+4 (justify): pattern-scan + patch the loaded libs in memory. Avoid
    // force-loading the targets when both optional patches are disabled.
    bool justify_enabled = ntf_global_config_bool("ntf_justify_kospan", true)
        || ntf_global_config_bool("ntf_justify_punct", true);
    if (justify_enabled) {
        ntf_forceload();
        for (size_t i = 0; i < sizeof(NTF_JUSTIFY_FIXES) / sizeof(NTF_JUSTIFY_FIXES[0]); i++) {
            if (!ntf_apply_justify_fix(&NTF_JUSTIFY_FIXES[i])) {
                // NickelHook's rename-back worker is not created until
                // ntf_init returns. Reboot instead of returning an error so
                // the .failsafe name remains in place for the next boot;
                // do not run destructors against an unknown code page. A plain
                // process exit is not sufficient because Nickel is not
                // guaranteed to be supervised and restarted on every firmware.
                NTF_LOG("CRITICAL: rebooting before the boot failsafe is disarmed.");
                nh_dump_log();
                sync();
                execl("/sbin/reboot", "reboot", (char *)NULL);
                NTF_LOG("CRITICAL: firmware reboot command failed: %s; trying the kernel reboot syscall.", strerror(errno));
                if (reboot(RB_AUTOBOOT) != 0)
                    NTF_LOG("CRITICAL: kernel reboot failed: %s; terminating the unsafe Nickel process.", strerror(errno));
                _exit(1);
            }
        }
    }
    return 0;
}

// ================= uninstall / wiring =================
// Delete one known installation artifact while treating an already-missing
// path as success. NickelHook performs the actual file operation.
static bool ntf_del(const char *p) { return (access(p, F_OK) && errno == ENOENT) ? true : nh_delete_file(p); }
static bool ntf_uninstall() {
    // NickelHook invokes this when the uninstall marker requests removal. The
    // in-memory patches need no undo step because they disappear with the
    // process; only on-device plugin files are removed here.
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

// ================= FIX 7: capital spacing (cpsp) (libnickel / QFontDatabase) =================
// Kobo's reader (Qt 5.2, optimizeLegibility on) shapes through the OLD HarfBuzz, which applies a
// font's default-LangSys GPOS features wholesale — including 'cpsp' (Capital Spacing). cpsp is meant
// only for all-caps runs, so in mixed-case body text it shoves every capital away from its neighbour
// (the loose "D" in "Docks"). The stripped shaper can't be made to gate it correctly, but we can drop
// cpsp from the font itself as it loads, for ANY font: hook QFontDatabase::addApplicationFont (the
// call FontManager uses to register every reader font — core, system, sideloaded), read the file,
// zero each cpsp feature's lookup count in memory, and register the edited bytes instead. 'case',
// 'kern', and everything else are untouched. Fail-safe throughout: on any problem the original font
// loads unchanged.

static bool ntf_cpsp_fix() { return ntf_global_config_bool("ntf_cpsp_fix", true); }

// Big-endian accessors (sfnt tables are big-endian). Every read is bounds-checked by the caller.
static inline uint16_t ntf_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t ntf_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Strip GPOS 'cpsp' in place by zeroing each cpsp Feature table's LookupIndexCount, so it applies no
// lookups. This leaves 'case'/'kern'/every other feature byte-for-byte intact and needs no table
// re-serialization. Returns true if anything changed. Every offset is bounds-checked against the
// GPOS table and the buffer; any inconsistency returns without touching the font, and the caller
// then loads it unchanged. Works because the old shaper applies default-LangSys GPOS wholesale, so
// an empty cpsp feature is simply a no-op.
static bool ntf_strip_cpsp(uint8_t *data, size_t len) {
    if (!data || len < 12) return false;
    uint32_t sfnt = ntf_be32(data);
    // Single sfnt fonts only (TTF 0x00010000, 'OTTO', 'true', 'typ1'); skip collections and unknowns.
    if (sfnt != 0x00010000u && sfnt != 0x4F54544Fu && sfnt != 0x74727565u && sfnt != 0x74797031u)
        return false;
    uint16_t num_tables = ntf_be16(data + 4);
    if (12 + (size_t)num_tables * 16 > len) return false;   // table directory: 16 bytes each from off 12
    uint32_t gpos_off = 0, gpos_len = 0;
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = data + 12 + (size_t)i * 16;
        if (ntf_be32(rec) == 0x47504F53u) { gpos_off = ntf_be32(rec + 8); gpos_len = ntf_be32(rec + 12); break; }  // 'GPOS'
    }
    if (!gpos_off || gpos_len < 10 || (size_t)gpos_off + gpos_len > len) return false;
    const uint8_t *gpos = data + gpos_off;
    uint16_t feat_list_off = ntf_be16(gpos + 6);   // GPOS header: version(4) scriptListOff(2) featureListOff(2)
    if (feat_list_off < 10 || (size_t)feat_list_off + 2 > gpos_len) return false;
    const uint8_t *flist = gpos + feat_list_off;
    uint16_t feat_count = ntf_be16(flist);
    if ((size_t)feat_list_off + 2 + (size_t)feat_count * 6 > gpos_len) return false;   // FeatureRecords: 6 bytes each
    bool changed = false;
    for (uint16_t i = 0; i < feat_count; i++) {
        const uint8_t *rec = flist + 2 + (size_t)i * 6;   // FeatureRecord: tag(4) + featureOffset(2, rel to FeatureList)
        if (ntf_be32(rec) != 0x63707370u) continue;       // 'cpsp'
        size_t ft = (size_t)feat_list_off + ntf_be16(rec + 4);   // Feature table, relative to GPOS start
        if (ft + 4 > gpos_len) continue;                  // malformed record: skip it, keep the rest
        uint8_t *lic = data + gpos_off + ft + 2;          // Feature table: featureParams(2) + lookupIndexCount(2)
        if (ntf_be16(lic) != 0) { lic[0] = 0; lic[1] = 0; changed = true; }
    }
    return changed;
}

// addApplicationFont is static int(const QString&); via NickelHook it's a plain int(const QString*).
static int (*real_addApplicationFont)(const QString *) = nullptr;

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
    { .sym = "_ZN15KepubBookReaderD1Ev", .sym_new = "_ntf_kepubReaderDtor",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kepubReaderDtor), .desc = "fix 5: clear destroyed reader state", .optional = true },
    { .sym = "_ZN10WebkitView12addCssToHtmlE7QString", .sym_new = "_ntf_wv_addCssToHtml",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_addCssToHtml), .desc = "arm reader-font re-apply", .optional = true },
    { .sym = "_ZN10WebkitView14setCurrentPageEi", .sym_new = "_ntf_wv_setCurrentPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_setCurrentPage), .desc = "re-apply reader font per chapter", .optional = true },
    // (letter-spacing on spaces is an in-memory byte patch, not a hook — see NTF_JUSTIFY_FIXES.)
    // FIX 7 — capital spacing: strip cpsp from each reader font as it's registered. Optional; a
    // missing symbol just sits the fix out. QFontDatabase::addApplicationFont is a Qt import in
    // libnickel's PLT, hooked the same way as FT_Load_Glyph in libkobo.
    { .sym = "_ZN13QFontDatabase18addApplicationFontERK7QString", .sym_new = "_ntf_addApplicationFont",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_addApplicationFont), .desc = "fix 7: strip cpsp per font at load", .optional = true },
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
    // This ABI thunk is the runtime proof that WebkitView is the +24 subobject
    // on the firmware being patched.  Without it, Fix 5 stays inert safely.
    { .name = "_ZThn24_N15KepubBookReaderD1Ev", .out = nh_symoutptr(ntf_kepubReaderWebkitDtorThunk), .desc = "fix 5: validate reader WebkitView subobject offset", .optional = true },
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
    if (__atomic_load_n(&ntf_hint_disabled, __ATOMIC_RELAXED)
        || ntf_hint_marker_state() != NTF_HINT_MARKER_ABSENT)
        return real_FT_Load_Glyph(face, glyph_index, load_flags);
    // Orthogonal to iType's CSM stem-weighting (Font Weight) — that's set before the load.
    FT_Int32 eff = load_flags;
    if (ntf_enabled() && ntf_no_hinting() && !ntf_font_hinting_allowed(face))
        eff |= NTF_FT_LOAD_NO_HINTING;
    return real_FT_Load_Glyph(face, glyph_index, eff);
}

// FIX 5 + FIX 2 — per-book reset. Clear the previous identities before calling Nickel, but do not
// publish the new reader until its real constructor has completed: a constructor-time WebkitView
// callback must not be able to re-enter Fix 5 with a partially constructed KepubBookReader.
extern "C" __attribute__((visibility("default")))
void _ntf_kepubReaderCtor(void *self, void *pluginState, void *widget) {
    bool on_qt = ntf_on_qt_thread();
    if (on_qt) {
        ntf_kepub_reader = nullptr;
        ntf_kepub_reader_view = nullptr;
        ntf_chapter_view = nullptr;
        ntf_chapter_needs_fix = false; // Fix 5: re-armed by each chapter's font-CSS injection
        ntf_fontfix_logged = false;    // let Fix 5 log its one friendly note again for this book
        ntf_vert_views_flush();        // Fix 2: stale per-view state must not survive into a new book
    }
    // A NULL real constructor is unrecoverable (there is nothing to construct
    // with) but also unreachable: NickelHook only installs a hook whose symbol
    // resolved. The guard exists purely to avoid a jump through NULL.
    if (!real_kepubReaderCtor) return;
    real_kepubReaderCtor(self, pluginState, widget);
    if (on_qt) {
        ntf_kepub_reader = self;   // complete KepubBookReader; now safe for Fix 5 to call
        // The +24 layout holds only on firmware where the resolve-time thunk
        // proved it; otherwise the view stays unknown until the first font-CSS
        // injection learns it (ntf_learn_reader_view).
        ntf_kepub_reader_view = ntf_kepubReaderWebkitDtorThunk
            ? (void *)((char *)self + NTF_KEPUB_WEBKIT_OFFSET) : nullptr;
    }
}
// The destructor is the lifetime boundary for the opaque reader pointer above.
// Clearing state before calling Nickel's destructor means no later WebkitView
// callback can mistake a freed reader for the active book.
extern "C" __attribute__((visibility("default")))
void _ntf_kepubReaderDtor(void *self) {
    if (self == ntf_kepub_reader) {
        // Clear even on a wrong thread (ntf_on_qt_thread still logs the
        // anomaly): a dangling reader pointer is strictly worse than the race
        // being reported.
        (void)ntf_on_qt_thread();
        ntf_kepub_reader = nullptr;
        ntf_kepub_reader_view = nullptr;
        ntf_chapter_view = nullptr;
        ntf_chapter_needs_fix = false;
        ntf_fontfix_logged = false;
    }
    if (real_kepubReaderDtor) real_kepubReaderDtor(self);
}
extern "C" __attribute__((visibility("default")))
void _ntf_cwv_setWritingDirection(void *self, int dir) {
    if (ntf_enabled() && ntf_vertfix() && ntf_vertfix_ready && ntf_on_qt_thread()) try {
        bool vert = (dir == ntf_wd_vrl || dir == ntf_wd_vlr);
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
    } catch (...) {
        // Contain Qt allocation failures: an OOM inside this cosmetic repair
        // must degrade to stock rendering, not unwind into Nickel's frames
        // (an exception escaping an extern "C" hook ends in std::terminate).
        NTF_LOG("Note: the vertical-text fix skipped one update after an internal error (likely low memory).");
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
    try {
        // WebkitView is shared by dictionary/store/browser views.  Only a call on
        // the current KepubBookReader may arm Fix 5; otherwise a later page change
        // could route a non-reader event into the reader-font methods. On firmware
        // where the +24 layout is unproven the reader's view starts out unknown
        // and the first injection while a reader is live tries to learn it.
        if (ntf_enabled() && ntf_kepub_fontfix() && real_kepubReaderDtor && !ntf_in_fixonturn
            && ntf_on_qt_thread()
            && (ntf_kepub_reader_view == self
                || (!ntf_kepub_reader_view && ntf_learn_reader_view(self)))) {
            ntf_chapter_needs_fix = true;
            ntf_chapter_view = self;
        }
        // FIX 2: this call REPLACES the view's whole user-stylesheet slot (it re-encodes `css` as a
        // data: URL and hands it to setUserStyleSheetUrl — see ntf_vert_views), which would wipe a
        // previously-set vertical override. If the injection is bound for a view we know is vertical,
        // carry the override inside the injected CSS so both survive in the one slot. `css` is this
        // call's own by-value copy (a caller-owned temporary per the ARM C++ ABI), so appending here
        // only affects this call.
        if (ntf_enabled() && ntf_vertfix() && ntf_vertfix_ready && ntf_wv_webView && css
            && ntf_on_qt_thread()) {
            void *cwv = ntf_wv_webView(self);
            bool tracked = cwv && ntf_vert_view_tracked(cwv);
            bool append = tracked && !css->contains(QString::fromLatin1(NTF_VERT_RULE));
            NTF_DBG("addCssToHtml wv=%p cwv=%p tracked=%d append=%d", self, cwv, tracked ? 1 : 0, append ? 1 : 0);
            if (append) css->append(QLatin1Char('\n')).append(QString::fromLatin1(NTF_VERT_RULE));
        }
    } catch (...) {
        // Contain Qt allocation failures (see _ntf_cwv_setWritingDirection);
        // the injection then goes through unmodified, which is stock behavior.
        NTF_LOG("Note: a CSS-injection fix skipped one update after an internal error (likely low memory).");
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
    // Consume only on the same live reader/view that armed the flag.  The
    // destructor normally clears ntf_kepub_reader; the identity checks also
    // make a missing destructor hook fail safe by sitting Fix 5 out.
    if (ntf_enabled() && ntf_kepub_fontfix() && real_kepubReaderDtor && ntf_chapter_needs_fix
        && self == ntf_chapter_view && self == ntf_kepub_reader_view && !ntf_in_fixonturn
        && ntf_pageStyleCss && ntf_kbr_addCssToHtml && ntf_on_qt_thread()) {
        ntf_chapter_needs_fix = false;
        ntf_chapter_view = nullptr;
        ntf_in_fixonturn = true;
        try {
            ntf_do_reinject(ntf_kepub_reader, page);
        } catch (...) {
            // Contain Qt allocation failures: skipping one chapter's re-apply
            // just leaves that chapter with stock behavior.
            NTF_LOG("Note: the reader-font fix skipped one chapter after an internal error (likely low memory).");
        }
        ntf_in_fixonturn = false;
    }
}

// FIX 7 — capital spacing. Intercept every reader-font registration, drop cpsp from the font in
// memory, and register the edited bytes via addApplicationFontFromData. Best-effort: on ANY problem
// we call the real addApplicationFont with the original path, so a font always loads. Only fonts we
// actually change take the from-data path (minimal blast radius); everything else loads stock. The
// try/catch contains Qt allocation failures — an exception escaping an extern "C" hook would
// std::terminate Nickel.
extern "C" __attribute__((visibility("default")))
int _ntf_addApplicationFont(const QString *fileName) {
    if (!real_addApplicationFont) return -1;
    if (!ntf_enabled() || !ntf_cpsp_fix() || !fileName) return real_addApplicationFont(fileName);
    try {
        QByteArray path = fileName->toLocal8Bit();
        FILE *f = fopen(path.constData(), "rb");
        if (!f) return real_addApplicationFont(fileName);   // Qt resource path / unreadable: stock load
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return real_addApplicationFont(fileName); }
        long sz = ftell(f);
        if (sz <= 0 || sz > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f); return real_addApplicationFont(fileName);
        }
        QByteArray buf;
        buf.resize((int)sz);
        size_t got = fread(buf.data(), 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) return real_addApplicationFont(fileName);
        if (!ntf_strip_cpsp(reinterpret_cast<uint8_t *>(buf.data()), (size_t)sz))
            return real_addApplicationFont(fileName);   // no cpsp: keep the stock file path
        int id = QFontDatabase::addApplicationFontFromData(buf);
        if (id < 0) return real_addApplicationFont(fileName);   // rejected: fall back to stock
        NTF_DBG("cpsp: stripped Capital Spacing from %s (app font id %d)", path.constData(), id);
        return id;
    } catch (...) {
        NTF_LOG("Note: the capital-spacing fix skipped one font after an internal error (likely low memory).");
        return real_addApplicationFont(fileName);
    }
}
