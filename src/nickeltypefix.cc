// NickelTypeFix — fixes several text-rendering defects in Kobo's Qt 5.2 WebKit/iType reader
// stack. Each fix is INDEPENDENT and FAIL-SAFE: it engages only if its seam is present on the
// running firmware, and a mismatch on one leaves the others working (nothing is written to disk;
// a boot without the mod is stock). Firmware 4.x only — inert on 5.x (NickelHook won't load).
//
//   1. Glyph "wobble"       — hook FT_Load_Glyph (libkobo), load unhinted   [ntf_no_hinting]
//   2. Vertical (tategaki)  — CSS override on the live page (libnickel)      [ntf_vertfix]
//   3. Justify: koboSpan     — in-memory patch, QTextEngine::justify (libQtGui)   [ntf_justify_kospan]
//   4. Justify: punctuation  — in-memory patch, isInterIdeographExpansionTarget (libQtWebKit) [ntf_justify_punct]
//   5. Letter-spacing spaces — in-memory patch, QTextEngine::shapeText (libQtGui)      [ntf_letterspace_spaces]
//   6. Reader-font fallback  — re-apply the reader-font CSS per kepub chapter (libnickel)   [ntf_kepub_fontfix]
//   7. Capital spacing (cpsp) — strip the cpsp feature per font at load, any font (libnickel/QFontDatabase)  [ntf_cpsp_fix]
//   8. Reader-font quoting   — quote the injected reader-font family so digit-token names hold (libnickel)   [ntf_quote_fontfamily]
//   9. Page-boundary clipping — stage 1: observe-only logging of the kepub pagination seams
//      [ntf_pagecut_probe, default OFF]; stage 2: clamp a boundary that cuts through a line
//      into the measured ink gap (libnickel)   [ntf_pagecut_fix, default OFF]
//
// Cause + fix for each is documented in ABOUT.md. Fixes 1, 2, and 6–9 use NickelHook PLT hooks;
// fixes 3–5 patch stripped device libs in memory (locate lib -> position-independent pattern-scan
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
#include <QRect>
#include <QVector>

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

# Fix 5 - letter-spacing on spaces: when text uses letter-spacing (tracking), Kobo widens the letters
# but not the spaces, so a tracked title runs its words together. This gives the spaces the same
# tracking so words stay apart. 0 = off.
ntf_letterspace_spaces:1

# Fix 6 - reader-font fallback: in a kepub book, a chapter's text sometimes renders in the system
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

# Fix 8 - reader-font quoting: in a kepub book, Kobo injects your reading font with an unquoted CSS
# rule. If the font's family name has a word that starts with a digit (e.g. "Roboto 2", "Bitter 24pt"),
# that rule is invalid CSS and the reader silently falls back to its default font. This quotes the
# family name so such fonts apply. A normal font name is unaffected. 0 = off.
ntf_quote_fontfamily:1

# Fix 9, stage 1 - page-boundary probe (diagnostic, off by default): log how the kepub reader places
# each page boundary, for the "characters cut off at page edges" investigation. Observation only:
# with this on, pages are still cut exactly as stock; only the log gains detail. Leave it at 0
# unless you are collecting a log for that issue.
ntf_pagecut_probe:0

# Fix 9, stage 2 - page-boundary clipping fix (off by default while field validation runs): in a
# kepub, a page boundary can land inside the letters of a line, shaving the tops of the next page's
# first line (the missing ink paints at the bottom of the previous page). With this on, a boundary
# that cuts through a line of your sideloaded reading font is moved into the empty gap between that
# line's letters and the next line's. It measures the font's real letter shapes to find that gap and
# leaves every boundary it cannot prove safe exactly as stock. Needs a sideloaded (not built-in)
# reading font selected in the reader.
ntf_pagecut_fix:0

# Verbose logging to nickel-type-fix.log. Off by default: a healthy boot logs nothing. Problems (a fix
# that can't apply, a failed write, a safety trip) are always logged regardless, and a problem in this
# file (a misspelled setting, an invalid value) turns verbose logging on automatically for that boot.
# 1 = log everything.
ntf_log:0
)CFG";

// The valid config keys, their baked-in default values, and a one-line description each. Kept
// directly below the default config above so the two can't drift apart: a key added there must be
// added here. This table drives the parser's unknown-key warning and the self-heal in config.c that
// appends keys a newer version added to an existing file. Each value here MUST match the default the
// code reads for that key (see the ntf_*_config_bool/get call sites): every fix defaults on, ntf_log
// off. The compact comments come from res/doc.
extern "C" const ntf_config_key_t ntf_config_keys[] = {
    { "ntf_enabled",            "1", "master switch; 0 = do nothing" },
    { "ntf_no_hinting",         "1", "Fix 1 - glyph \"wobble\" fix; 0 = stock rendering" },
    { "ntf_hinting_allowlist",  "",  "comma-separated families to leave hinted, e.g. Georgia, Kobo Nickel" },
    { "ntf_vertfix",            "1", "Fix 2 - vertical (tategaki) text fix" },
    { "ntf_justify_kospan",     "1", "Fix 3 - justified-kepub boundary fix (the main justify fix)" },
    { "ntf_justify_punct",      "1", "Fix 4 - punctuation justification fix" },
    { "ntf_letterspace_spaces", "1", "Fix 5 - give spaces the same letter-spacing as the letters (tracked text)" },
    { "ntf_kepub_fontfix",      "1", "Fix 6 - reader-font fallback fix; re-applies the reading font per kepub chapter" },
    { "ntf_cpsp_fix",           "1", "Fix 7 - strip cpsp so capitals aren't spaced apart in body text (any font)" },
    { "ntf_quote_fontfamily",   "1", "Fix 8 - quote the injected reader-font family so digit-token names apply" },
    { "ntf_pagecut_probe",      "0", "Fix 9 stage 1 - log kepub page-boundary placement (diagnostic; pages are cut exactly as stock)" },
    { "ntf_pagecut_fix",        "0", "Fix 9 stage 2 - keep kepub page boundaries out of the letters (needs a sideloaded reading font)" },
    { "ntf_log",                "0", "verbose per-fix log to nickel-type-fix.log; off by default" },
    { NULL, NULL, NULL },
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
// Fix 6: reader-font fallback repair (on by default).
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

// ================= FIX 6: reader-font fallback repair (libnickel) =================
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
// Fix 6 uses a raw pointer because NickelHook exposes C++ objects as opaque
// addresses.  KepubBookReader is multiple-inherited, so the WebkitView hooks'
// `self` need not equal the complete-object pointer received by the
// constructor; the view pointer is therefore LEARNED per book from the live
// objects (ntf_learn_reader_view), never assumed.  The destructor clears both
// identities before the object can disappear, and an unprovable layout leaves
// the fix inert for the book rather than calling a wrong object.
//
// History, because this was got wrong once: v0.5–v0.7 assumed WebkitView was
// the +24 subobject, taking the mere existence of the `_ZThn24_N15KepubBook-
// ReaderD1Ev` thunk as proof.  That thunk is real but belongs to a DIFFERENT
// base at +24 (the one carrying the contentMargins/setAutoHighlightStyle
// overrides).  On 4.45.23697, KepubBookReaderBase::locatePages has no
// this-adjusting thunk at all, so WebkitView is the PRIMARY base at offset 0
// (confirmed live: the pagination probe logged the reader's WebkitView equal
// to the KepubBookReader pointer itself).  The +24 assumption made the
// identity gate unmatchable and left Fix 6 armed never — see CHANGELOG v0.8.
// Also note the originally sketched "exactly one shared adjustment between
// the KepubBookReader and WebkitView thunk sets" heuristic does NOT work:
// KepubBookReader emits destructor thunks at N = 8, 24, 216, 236, and 520
// (several non-primary bases), and the set shared with WebkitView is {8, 24}
// — not unique.
static void *ntf_kepub_reader = nullptr;
static void *ntf_kepub_reader_view = nullptr;
static void *ntf_chapter_view = nullptr;            // view that armed the pending chapter repair
static bool ntf_in_fixonturn = false;               // re-entrancy guard around the re-inject
static bool ntf_chapter_needs_fix = false;          // armed by a reader CSS injection, consumed by that view
static bool ntf_fontfix_logged = false;             // the friendly "fix is active" note, once per book

// GUI-thread guard for the Qt-side hooks. All Fix 2/6 state above is plain
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
// `reader` is the complete KepubBookReader object; the WebkitView hooks' `self`
// is the reader's WebkitView base subobject, whatever its offset in the object.
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

// Learn the reader's view: called from the WebkitView CSS-injection hook while
// a reader is live and no view has been learned for it yet. `self` is the
// WebkitView receiving addCssToHtml; it is the reader's own view iff it is a
// base subobject of the live KepubBookReader. Prove that from the C++ ABI
// instead of assuming a layout, per candidate offset:
//   - offset 0 (WebkitView is the primary base — the 4.45.23697 layout): the
//     un-thunked complete-object destructor `_ZN15KepubBookReaderD1Ev` must
//     appear in the vtable the candidate points at. Only the reader's own
//     primary subobject carries that vtable.
//   - offset > 0: a matching this-adjusting destructor thunk `_ZThn<off>_...`
//     must exist in libnickel AND appear in the candidate subobject's vtable —
//     which ties the offset to this object's dynamic type, not to a lookalike
//     heap neighbour (a different complete object's vtable never contains
//     another class's `_ZThn` destructor thunks).
// Every failure path leaves Fix 6 inert for the book; this can delay the fix
// on an unknown firmware but can never aim a call at the wrong object.
// GUI-thread only (callers hold the guard).
static bool ntf_learn_reader_view(void *self) {
    if (!ntf_kepub_reader || !self) return false;
    uintptr_t base = (uintptr_t)ntf_kepub_reader, cand = (uintptr_t)self;
    if (cand < base) return false;
    uintptr_t off = cand - base;
    if (off > 1024 || off % 4 != 0) return false;      // sane single-object layouts only
    void *proof;                                       // the destructor entry the vtable must carry
    if (off == 0) {
        // The primary-base case has no thunk; the proof is the complete-object
        // destructor itself, already resolved for the destructor hook.
        proof = (void *)real_kepubReaderDtor;
    } else {
        char sym[64];
        int n = snprintf(sym, sizeof(sym), "_ZThn%u_N15KepubBookReaderD1Ev", (unsigned)off);
        if (n < 0 || (size_t)n >= sizeof(sym)) return false;
        static void *libnickel = dlopen("libnickel.so.1.0.0", RTLD_LAZY | RTLD_NOLOAD);
        if (!libnickel) return false;
        proof = dlsym(libnickel, sym);
    }
    if (!proof) return false;
    // The destructor sits within the first few virtual slots for every class in
    // this hierarchy; 12 bounds the scan while staying inside the vtable.
    void **vtable = *(void ***)self;
    if (!vtable) return false;
    for (int i = 0; i < 12; i++) {
        if (vtable[i] == proof) {
            ntf_kepub_reader_view = self;
            NTF_DBG("Reader-font fix: discovered the reader's view at offset +%u on this firmware.", (unsigned)off);
            return true;
        }
    }
    return false;
}

// ================= FIX 9, stage 1: kepub page-boundary probe (libnickel) =================
// Observation only — this stage changes NOTHING about how pages are cut. Background: in a kepub,
// WebkitView::locatePages builds the page table from one rect per text run: it collects the run
// rects, sorts them (WebkitView::sortRectsByStart), merges adjacent ones, and walks them page by
// page. The static WebkitView::cutPage(rects, start, limit, dir) — which returns the deepest
// run-rect end that still fits the limit, or the raw limit when none fits — is called only on
// the walk iterations where a rect straddles the page limit; when the last fitting rect ends
// inside the limit, the walk places the boundary without cutPage. Either way the boundary can
// land inside real glyph ink, shaving the tops or bottoms of letters at page edges (the "cut off
// characters" report). The corrective stage needs facts only a live reader can provide (how
// often each boundary path runs, the real font-size units, the actual rect geometry per
// firmware), so this stage hooks the pagination seams as pure passthroughs: call the real
// function, log what went in and out, and return the value UNCHANGED. With the probe off (the
// default) the hooks are inert beyond a config check; with it on, pagination is still
// byte-identical to stock — only the log differs.
//
// Seams, and what each one can and cannot see (v1 of this probe got this wrong and burned a
// hardware session on it; the facts below are from the 4.45.23697 disassembly plus that
// session's log):
//   - locatePages is VIRTUAL. The reader reaches KepubBookReaderBase::locatePages through a
//     vtable slot (R_ARM_ABS32 relocations in the vtables), which a PLT hook never sees; the
//     only PLT calls to it are two annotation-refresh sites. So the KepubBookReaderBase hook
//     below almost never fires and `reader=` in the begin line is 0 for a normal reader pass.
//     The reader is identified instead by comparing the pass's view against Fix 6's tracked
//     reader view (`readerView=`/`match=` in the begin line).
//   - WebkitView::locatePages IS reliably PLT-visible: the KepubBookReaderBase override's first
//     act is a PLT call to the base (0x00b7f7de -> stub 0x006ab0e0 -> the hooked JUMP_SLOT), and
//     the dictionary wrappers do the same, so every execution of the base body — the only body
//     that walks rects — is bracketed here no matter how the override itself was dispatched.
//   - sortRectsByStart is static and has exactly one caller, inside WebkitView::locatePages,
//     past the early exits (pending stylesheets, the fixed-layout single-page path) and right
//     before the page walk. It marks a pass that did real pagination work and carries the full
//     line-rect vector, whether or not any iteration ends up needing cutPage.
//   - cutPage is static (no `this`) and its only two call sites are the straddle iterations
//     inside WebkitView::locatePages, both through the PLT stub that reads the hooked JUMP_SLOT.
//
// The begin line also logs the view's fontSize() for the pass (per the disassembly it returns
// the raw reading-font-size setting; whether that is the rendered pixel size is exactly what
// the logged rect geometry settles), and the end line logs totalPages() — the size of the page
// table the pass left behind — so a pass that paginates without ever cutting is still measured.
//
// The probe's second hardware run (Clara BW, 4.45.23697) proved the no-cut path is the normal
// one: ten real pagination passes (sorted=1, pages=4..5), zero cutPage calls, zero strays. So
// the boundary decision cannot be observed from inside cutPage at all; instead, each pass-end
// reads the placed boundaries back out of the page table (via the exported getPageOffset
// accessor) and classifies every boundary against the cached sorted rects: does it sit on a
// rect's top (the no-cut walk breaking at a line start, leaving zero slack above that line's
// ink — the leading suspect for the clipped-ascender defect), on a rect's end (the cutPage
// rule), or elsewhere. That classification is the measurement the corrective stage is gated on.
static int (*real_wv_cutPage)(const QVector<QRect> *rects, int start, int limit, int dir) = nullptr;
static void *(*real_wv_locatePages)(void *self, int reload) = nullptr;
static void *(*real_kbrb_locatePages)(void *self, int reload) = nullptr;
// sortRectsByStart sorts the vector in place; its return value (if any) is passed through as raw
// r0 so the hook is transparent even if the true return type is not void on some firmware.
static void *(*real_wv_sortRects)(QVector<QRect> *rects, int dir) = nullptr;
static int (*ntf_wv_fontSize)(void *self) = nullptr;     // dlsym'd; null-checked at the use site
static int (*ntf_wv_totalPages)(void *self) = nullptr;   // dlsym'd; a two-load member read (page-table size)
// dlsym'd WebkitView::getPageOffset(int, int&, int&) const: for the 1-BASED page index it writes
// the page's start and end offsets out of the page table the pass just built and returns whether
// the index was valid. This is the readout for the boundary lines: the start of page p (p >= 2)
// IS the boundary the walk chose. First firmware with the symbol is 4.25.15875; on older
// firmware it stays null and the probe logs everything except the boundary lines.
static int (*ntf_wv_getPageOffset)(void *self, int page, int *start, int *end) = nullptr;

static bool ntf_pagecut_probe() { return ntf_global_config_bool("ntf_pagecut_probe", false); }
static bool ntf_pagecut_fix();   // stage 2's key; defined with the fix, below the cpsp helpers

// Per-pass probe state. GUI-thread only (all writers hold ntf_on_qt_thread), so plain data is
// race-free the same way the Fix 2/6 state is.
static unsigned ntf_pagecut_pass = 0;        // pass id, monotonically increasing per boot
static int ntf_pagecut_depth = 0;            // locatePages nesting (reader wraps the base call)
static bool ntf_pagecut_from_reader = false; // outermost frame was KepubBookReaderBase::locatePages
static bool ntf_pagecut_begun = false;       // "pass N begin" line written
static int ntf_pagecut_cuts = 0;             // cutPage calls seen this pass
static int ntf_pagecut_sorted = 0;           // sortRectsByStart calls seen this pass (pass reached the walk)
static int ntf_pagecut_cls[4];               // per-classification tallies, indexed by ntf_pagecut_cls_t
static int ntf_pagecut_logged_edge = 0;      // per-cut "edge" lines written this pass
static int ntf_pagecut_logged_other = 0;     // per-cut non-"edge" lines written this pass
static int ntf_pagecut_suppressed = 0;       // per-cut lines dropped by the per-pass caps
static int ntf_pagecut_rect_dir = 0;         // the pass's WritingDirection, from the sort hook

// Cache of the pass's sorted line rects (top and end per rect, plain ints — no Qt in the probe's
// storage), filled by the sortRectsByStart hook and read back when the pass ends to classify each
// placed boundary against the line geometry. The first hardware run of this probe showed real
// passes paginating with ZERO cutPage calls, so the boundary decision must be observed from the
// resulting page table, not from inside a cut. GUI-thread only, like the rest of the pass state.
#define NTF_PAGECUT_RECTS_MAX 2048
static int ntf_pagecut_rect_tops[NTF_PAGECUT_RECTS_MAX];
static int ntf_pagecut_rect_ends[NTF_PAGECUT_RECTS_MAX];
static int ntf_pagecut_rect_n = 0;           // cached rects this pass (0 = none)
static bool ntf_pagecut_rect_trunc = false;  // vector was longer than the cache

// An event the pass bracket cannot own — a probe hook reached on a thread other than the claimed
// GUI thread, or a cutPage/sortRects call with no locatePages frame open — is logged as a
// self-contained "stray" line instead of being dropped. v1 dropped such events silently, which
// made "cutPage never ran" indistinguishable from "cutPage ran where the probe refused to look",
// and that ambiguity cost the first hardware session. A stray line reads only the hook's own
// arguments (owned by the caller's stack frame for the duration of the call, whatever the
// thread), never the pass state. Capped per boot; the pass-end `stray=` tally still counts every
// event past the cap.
#define NTF_PAGECUT_STRAY_MAX 64
static int ntf_pagecut_strays = 0;           // atomic (strays can arrive on any thread)
static bool ntf_pagecut_cut_seen = false;    // atomic; "cutPage never ran" becomes a positive statement
static bool ntf_pagecut_stray_ok(void) {
    int n = __atomic_add_fetch(&ntf_pagecut_strays, 1, __ATOMIC_RELAXED);
    if (n == NTF_PAGECUT_STRAY_MAX + 1)
        NTF_LOG("pagecut probe: stray-line cap reached; later strays are only counted (stray= in pass-end lines)");
    return n <= NTF_PAGECUT_STRAY_MAX;
}

// How the stock return relates to the rect vector. EDGE = the deepest rect end that fits the
// limit (the normal path); LIMIT_NOCAND = no rect fits, the raw limit came back; LIMIT_STALE =
// the deepest fitting end is at or before the start, the raw limit came back; UNMODELED = the
// return matches neither reconstruction (would mean the decode of cutPage's rule is wrong).
// The reconstruction reads the y axis; in a vertical (tategaki) book the real rule cuts on x, so
// those passes classify as "unmodeled" by design — the logged `dir` tells them apart.
enum ntf_pagecut_cls_t { NTF_CUT_EDGE = 0, NTF_CUT_LIMIT_NOCAND, NTF_CUT_LIMIT_STALE, NTF_CUT_UNMODELED };
static const char *const ntf_pagecut_cls_name[] = { "edge", "limit-nocand", "limit-stale", "unmodeled" };

// Cap the per-cut detail lines per pass so a long book can't flush the whole log through its
// 256 KB rotation in one open; the tallies in the pass-end line still count every call.
#define NTF_PAGECUT_LINES_MAX 64

// Append one rect as "(x,y wxh)" to a line buffer. Returns false when the buffer is full.
static bool ntf_pagecut_fmt_rect(char *buf, size_t bufsz, size_t *off, const QRect &r) {
    if (*off >= bufsz) return false;
    int w = snprintf(buf + *off, bufsz - *off, "(%d,%d %dx%d)", r.x(), r.y(), r.width(), r.height());
    if (w < 0 || (size_t)w >= bufsz - *off) return false;
    *off += (size_t)w;
    return true;
}

// Summarize a rect vector as its first three and last two rects. Read-only over the vector.
static void ntf_pagecut_fmt_rects(const QVector<QRect> *rects, char *buf, size_t bufsz) {
    int n = rects ? rects->size() : 0;
    size_t off = 0;
    bool ok = true;
    for (int i = 0; i < n && i < 3 && ok; i++)
        ok = ntf_pagecut_fmt_rect(buf, bufsz, &off, rects->at(i));
    if (n > 5 && off < bufsz - 4) {
        memcpy(buf + off, "...", 4);
        off += 3;
    }
    int tail_from = (n > 5) ? (int)((long long)n - 2) : 3;   // long long: no signed-overflow UB on untrusted n
    for (int i = tail_from; i < n && ok; i++)
        ok = ntf_pagecut_fmt_rect(buf, bufsz, &off, rects->at(i));
    if (bufsz) buf[(off < bufsz) ? off : bufsz - 1] = '\0';
}

// How one cutPage return relates to its rect vector. Pure over the arguments (no probe state),
// so it serves both the bracketed per-pass accounting and the stray lines.
struct ntf_pagecut_cut_info {
    ntf_pagecut_cls_t cls;
    long long best_top, best_h, best_end;   // the reconstructed boundary rect (EDGE only)
    long long adv;                          // top-to-next-top advance at the boundary, -1 if unknown
    int n;                                  // rect count, -1 for a null vector
};
static ntf_pagecut_cut_info ntf_pagecut_classify(const QVector<QRect> *rects, int start, int limit, int ret) {
    ntf_pagecut_cut_info ci = { NTF_CUT_UNMODELED, 0, 0, 0, -1, rects ? rects->size() : -1 };

    // Reconstruct the stock choice: the deepest end = y + height with end <= limit; ties keep
    // the first. Kept in step with the disassembled rule the corrective stage relies on, so a
    // firmware where the rule differs shows up as "unmodeled" in the log instead of a wrong fix.
    // 64-bit arithmetic: the rect fields are untrusted ints, so sums and differences must not be
    // able to overflow (which would also be UB the optimizer is free to exploit).
    bool have_best = false;
    for (int i = 0; i < ci.n; i++) {
        const QRect &r = rects->at(i);
        long long end = (long long)r.y() + r.height();
        if (end > limit) continue;
        if (!have_best || end > ci.best_end) {
            have_best = true;
            ci.best_end = end;
            ci.best_top = r.y();
            ci.best_h = r.height();
        }
    }
    if (!have_best)
        ci.cls = (ret == limit) ? NTF_CUT_LIMIT_NOCAND : NTF_CUT_UNMODELED;
    else if (ci.best_end <= start)
        ci.cls = (ret == limit) ? NTF_CUT_LIMIT_STALE : NTF_CUT_UNMODELED;
    else
        ci.cls = (ret == ci.best_end) ? NTF_CUT_EDGE : NTF_CUT_UNMODELED;

    // The line advance at the boundary: distance from the boundary rect's top to the next rect
    // top below it. Against the pass's fontSize this settles the size units (advance ≈ line
    // height × pixel size) without any model assumption.
    if (ci.cls == NTF_CUT_EDGE) {
        for (int i = 0; i < ci.n; i++) {
            long long d = (long long)rects->at(i).y() - ci.best_top;
            if (d > 0 && (ci.adv < 0 || d < ci.adv))
                ci.adv = d;
        }
    }
    return ci;
}

// Log one bracketed cutPage call: the raw arguments, the stock return, and how that return
// relates to the rect vector. Read-only over the vector; never touches the return value.
static void ntf_pagecut_observe(const QVector<QRect> *rects, int start, int limit, int dir, int ret) {
    ntf_pagecut_cuts++;
    ntf_pagecut_cut_info ci = ntf_pagecut_classify(rects, start, limit, ret);
    ntf_pagecut_cls[ci.cls]++;

    // Cap "edge" lines (the common case) and the rarer classifications separately, so a late
    // limit-fallback — the one the investigation most needs — is never crowded out by hundreds
    // of healthy cuts before it.
    int *logged = (ci.cls == NTF_CUT_EDGE) ? &ntf_pagecut_logged_edge : &ntf_pagecut_logged_other;
    if (*logged >= NTF_PAGECUT_LINES_MAX) {
        ntf_pagecut_suppressed++;
        return;
    }
    (*logged)++;

    // Rect-vector summary, once per pass. Normally the sortRectsByStart hook has already written
    // it (the sorted vector is the very one handed to cutPage); this covers a pass whose sort
    // call the probe somehow did not see.
    if (ntf_pagecut_cuts == 1 && ntf_pagecut_sorted == 0 && ci.n > 0) {
        char buf[192];
        ntf_pagecut_fmt_rects(rects, buf, sizeof(buf));
        NTF_LOG("pagecut probe: pass %u rects n=%d %s", ntf_pagecut_pass, ci.n, buf);
    }

    NTF_LOG("pagecut probe: pass %u cut %d: start=%d limit=%d dir=%d n=%d ret=%d cls=%s best=(top=%lld h=%lld end=%lld) adv=%lld",
        ntf_pagecut_pass, ntf_pagecut_cuts, start, limit, dir, ci.n, ret,
        ntf_pagecut_cls_name[ci.cls], ci.best_top, ci.best_h, ci.best_end, ci.adv);
}

// Bracket one locatePages frame. The outermost frame starts a pass; the innermost WebkitView
// frame carries the view identity (its `this` IS the WebkitView), so the begin line is written
// there. `reader_frame` marks the KepubBookReaderBase wrapper — but that wrapper is reached
// through a vtable slot on a normal reader pass (see the seam notes above), so `reader=1`
// appears only for the PLT-called annotation path. The reader identification that always works
// is `match=`: whether this pass's view is the live KepubBookReader itself (WebkitView is the
// primary base on the confirmed firmware) or the reader view Fix 6 has learned for the book.
// Both sides of that comparison are GUI-thread state, and the caller holds the guard.
static void ntf_pagecut_pass_enter(void *self, int reload, bool reader_frame) {
    if (ntf_pagecut_depth++ == 0) {
        ntf_pagecut_pass++;
        ntf_pagecut_from_reader = false;
        ntf_pagecut_begun = false;
        ntf_pagecut_cuts = 0;
        ntf_pagecut_sorted = 0;
        ntf_pagecut_logged_edge = 0;
        ntf_pagecut_logged_other = 0;
        ntf_pagecut_suppressed = 0;
        ntf_pagecut_rect_n = 0;
        ntf_pagecut_rect_trunc = false;
        ntf_pagecut_rect_dir = 0;
        memset(ntf_pagecut_cls, 0, sizeof(ntf_pagecut_cls));
    }
    if (reader_frame) {
        ntf_pagecut_from_reader = true;
    } else if (!ntf_pagecut_begun) {
        ntf_pagecut_begun = true;
        // fontSize() is the one Qt call in this function; contain it here so the depth
        // accounting above can never be skipped by an unwind (the hook's enter/leave pairing is
        // what keeps the pass bracket balanced).
        int fs = -1;
        if (ntf_wv_fontSize) try { fs = ntf_wv_fontSize(self); } catch (...) { fs = -2; }
        // Reader identity, from the probe's second hardware run: `view` came out equal to the
        // KepubBookReader pointer itself on every pass — WebkitView is the PRIMARY base
        // (offset 0) on 4.45.23697 (the finding that led to the Fix 6 gate repair; see the
        // history note at the Fix 6 state block). Match against the reader object itself AND
        // against Fix 6's learned view, and log both pointers, so whichever layout a firmware
        // has is visible rather than assumed.
        void *rd = ntf_kepub_reader;
        void *rv = ntf_kepub_reader_view;
        NTF_LOG("pagecut probe: pass %u begin view=%p reader=%p readerView=%p match=%d reload=%d fontSize=%d",
            ntf_pagecut_pass, self, rd, rv,
            ((rd && rd == self) || (rv && rv == self)) ? 1 : 0, reload, fs);
    }
}

// Relate one placed boundary to the pass's cached line rects. `b` is the start offset of page
// `page` as the walk stored it; `above` is the cached rect with the greatest top below b (the
// last line the previous page can show), `next` the one with the smallest top at or past b (the
// first line of the new page). cls: "top" = b sits exactly on a rect top (the no-cut walk
// placing the break at a line's start — zero slack above that line's ink), "cut" = b sits
// exactly on a rect end (the cutPage rule), "other" = neither, "vertical"/"uncached" = the
// geometry axis or the cache cannot support the comparison. -1 prints for a side with no rect.
static void ntf_pagecut_log_boundary(int page, int b) {
    const char *cls;
    int above_top = -1, above_end = -1, next_top = -1, next_end = -1;
    if (ntf_pagecut_rect_dir != 0) {
        cls = "vertical";   // the walk cuts on x for vertical text; y-axis rect matching would lie
    } else if (ntf_pagecut_rect_n == 0) {
        cls = "uncached";
    } else {
        bool top_hit = false, end_hit = false;
        for (int i = 0; i < ntf_pagecut_rect_n; i++) {
            int t = ntf_pagecut_rect_tops[i], e = ntf_pagecut_rect_ends[i];
            if (t == b) top_hit = true;
            if (e == b) end_hit = true;
            if (t < b && (above_top == -1 || t > above_top)) { above_top = t; above_end = e; }
            if (t >= b && (next_top == -1 || t < next_top)) { next_top = t; next_end = e; }
        }
        cls = top_hit ? "top" : end_hit ? "cut" : "other";
    }
    NTF_LOG("pagecut probe: pass %u boundary p%d: B=%d cls=%s%s above=(top=%d end=%d) next=(top=%d end=%d)",
        ntf_pagecut_pass, page, b, cls, ntf_pagecut_rect_trunc ? " (cache truncated)" : "",
        above_top, above_end, next_top, next_end);
}

static void ntf_pagecut_pass_leave(void *self) {
    if (--ntf_pagecut_depth > 0) return;
    if (ntf_pagecut_depth < 0) ntf_pagecut_depth = 0;   // unbalanced (probe toggled mid-pass): resync
    if (!ntf_pagecut_begun && ntf_pagecut_cuts == 0 && ntf_pagecut_sorted == 0) return;
    // totalPages() is a two-load member read of the page table the pass just left behind; it is
    // the outcome measurement for the (common) passes that paginate without ever calling
    // cutPage. Same containment as fontSize() above.
    int pages = -1;
    if (ntf_wv_totalPages) try { pages = ntf_wv_totalPages(self); } catch (...) { pages = -2; }
    // The boundary readout: page p's start offset (p >= 2) is a boundary the walk placed. Read
    // back through the exported getPageOffset accessor — no raw member offsets — and classify
    // each against the cached rect geometry. Capped at 32 lines per pass.
    if (ntf_wv_getPageOffset && pages > 1 && ntf_pagecut_sorted > 0) {
        int last = (pages > 33) ? 33 : pages;
        for (int p = 2; p <= last; p++) {
            int bs = 0, be = 0, ok = 0;
            try { ok = ntf_wv_getPageOffset(self, p, &bs, &be); } catch (...) { ok = 0; }
            if (!ok) break;
            ntf_pagecut_log_boundary(p, bs);
        }
        if (pages > last)
            NTF_LOG("pagecut probe: pass %u boundaries capped at %d of %d", ntf_pagecut_pass, last - 1, pages - 1);
    }
    NTF_LOG("pagecut probe: pass %u end cuts=%d sorted=%d pages=%d edge=%d limit-nocand=%d limit-stale=%d unmodeled=%d suppressed=%d stray=%d",
        ntf_pagecut_pass, ntf_pagecut_cuts, ntf_pagecut_sorted, pages, ntf_pagecut_cls[NTF_CUT_EDGE],
        ntf_pagecut_cls[NTF_CUT_LIMIT_NOCAND], ntf_pagecut_cls[NTF_CUT_LIMIT_STALE],
        ntf_pagecut_cls[NTF_CUT_UNMODELED], ntf_pagecut_suppressed,
        __atomic_load_n(&ntf_pagecut_strays, __ATOMIC_RELAXED));
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
    // Startup block, always logged: mod version, firmware version, effective config, and (below)
    // the resolved-symbol map. This is what makes a user-attached log diagnostic on an unknown
    // firmware, so it must not depend on ntf_log:1 or on the mod being enabled.
    NTF_LOG("startup: NickelTypeFix " NH_VERSION);
    ntf_log_firmware();
    NTF_LOG("startup: enabled=%d fixes(wobble/vertical/justify/readerfont)=%d/%d/%d/%d verbose=%d",
        ntf_enabled(), ntf_no_hinting(), ntf_vertfix(),
        (ntf_global_config_bool("ntf_justify_kospan", true) || ntf_global_config_bool("ntf_justify_punct", true)),
        ntf_kepub_fontfix(), ntf_log());

    if (!ntf_enabled()) { NTF_LOG("NickelTypeFix is turned off in its config (ntf_enabled:0); nothing was changed."); return 0; }

    // FIX 2 (vertical): learn the vertical-writing-mode enum values from Nickel itself.
    NTF_LOG("startup: vertical/reader syms cwvSetDir=%p cwvSettings=%p setUserCss=%p getUserCss=%p wvWebView=%p kepubCtor=%p kepubDtor=%p wdFromString=%p",
        (void *)real_cwv_setWritingDirection, (void *)ntf_cwv_settings, (void *)ntf_setUserStyleSheetUrl,
        (void *)ntf_getUserStyleSheetUrl, (void *)ntf_wv_webView, (void *)real_kepubReaderCtor,
        (void *)real_kepubReaderDtor, (void *)ntf_writingDirectionFromString);
    // FIX 9, stage 1 (probe): resolved seams + the config switch. Logged unconditionally like the
    // rest of the startup block, so an attached log shows whether the probe could attach.
    NTF_LOG("startup: pagecut probe=%d fix=%d syms cutPage=%p wvLocatePages=%p kbrbLocatePages=%p sortRects=%p wvFontSize=%p wvTotalPages=%p wvGetPageOffset=%p",
        ntf_pagecut_probe(), ntf_pagecut_fix(), (void *)real_wv_cutPage, (void *)real_wv_locatePages,
        (void *)real_kbrb_locatePages, (void *)real_wv_sortRects, (void *)ntf_wv_fontSize,
        (void *)ntf_wv_totalPages, (void *)ntf_wv_getPageOffset);
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
    ok = ntf_del(NTF_CONFIG_DIR "/nickel-type-fix.log.old") && ok;
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

// ============ FIX 9, stage 2: kepub page-boundary clipping fix (libnickel) ============
// Repairs the boundary defect the stage-1 probe measured on device: on the cutPage path the
// boundary is the deepest run-rect BOTTOM that fits the page limit, but consecutive line rects
// overlap (rect height = ascent + descent + ~0.135 em of slack, against a smaller line advance),
// so that bottom lands inside the NEXT line's rect — 3 px inside it on the measured firmware,
// on every single cut — and the next page starts with the tops of its first line's letters
// shaved off (they paint at the bottom of the previous page instead). The fix clamps cutPage's
// return into the measured INK GAP between the two lines around the boundary: no lower than the
// previous line's deepest ink, no deeper than the next line's tallest ink. A healthy boundary
// (already inside the gap) is returned unchanged, so pagination only moves where it was cutting
// through letters.
//
// Scope, deliberately narrow (v1):
//   - Only the cutPage path is adjusted. The no-cut path (boundary at a line's rect top) leaves
//     at most the sub-pixel ascender overhang of the measured fonts and is left stock.
//   - Only horizontal dir==0 passes; vertical and RTL sit out.
//   - Only line pairs whose rect heights match the active font's predicted height (±2 px), so
//     headings, images, and mixed-metric lines keep their stock boundary.
//   - Ink extents come from the font file itself (glyf outline bounding boxes over a fixed
//     repertoire), measured once at registration; a font that cannot be measured (CFF/OTTO,
//     collections, QTD-compressed core fonts, no cmap format 4) leaves the fix inert for that
//     family. The fix never guesses: every unprovable case returns the stock boundary.
//
// Determinism: the clamp is a pure function of (rect vector, start, limit, stock return) and an
// armed snapshot taken once per pagination pass, so the same layout always produces the same
// page table. The stall-loop hazard (a return <= start would hang locatePages) is guarded
// explicitly; on any doubt the stock value is returned.
//
// Key: ntf_pagecut_fix, DEFAULT OFF while stage-2 field validation is running. The stage-1
// probe (ntf_pagecut_probe) is independent: it observes the raw cut before this clamp and the
// finished page table after it, so a probe log shows both the stock decision and the repair.
static bool ntf_pagecut_fix() { return ntf_global_config_bool("ntf_pagecut_fix", false); }

static bool ntf_css_generic_family(const QString &value);   // defined with Fix 8 below

// --- per-family font metrics, measured at registration time ---
// Filled by ntf_pagecut_register_font (called from the addApplicationFont hook, which already
// reads every reader font file). Font units throughout; converted to pixels at arm time.
#define NTF_FONTMETRICS_MAX 64
#define NTF_FAMILY_MAX 64
struct ntf_fontmetrics_t {
    char family[NTF_FAMILY_MAX];  // casefolded family name (name table, ID 16 falling back to 1)
    uint16_t upem;                // head.unitsPerEm
    int asc;                      // hhea ascender (> 0)
    int desc;                     // hhea descender magnitude (>= 0)
    int inkAsc;                   // tallest outline top over the repertoire (> 0)
    int inkDesc;                  // deepest outline drop over the repertoire (>= 0)
    bool usable;
};
static ntf_fontmetrics_t ntf_fontmetrics[NTF_FONTMETRICS_MAX];
static int ntf_fontmetrics_n = 0;
static char ntf_pagecut_family[NTF_FAMILY_MAX];   // active reading family, casefolded; "" = none

// ASCII-fold a family name for comparisons: lower-case A–Z, bytes above 0x7F kept as-is (the
// same transform is applied to both the name-table string and the CSS value, so the comparison
// is consistent without any locale machinery).
static void ntf_family_fold(const char *src, size_t n, char out[NTF_FAMILY_MAX]) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < NTF_FAMILY_MAX; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

static ntf_fontmetrics_t *ntf_fontmetrics_find(const char *folded) {
    for (int i = 0; i < ntf_fontmetrics_n; i++)
        if (strcmp(ntf_fontmetrics[i].family, folded) == 0)
            return &ntf_fontmetrics[i];
    return nullptr;
}

// Locate one sfnt table. Returns NULL unless the table lies entirely inside the buffer.
static const uint8_t *ntf_sfnt_table(const uint8_t *d, size_t len, uint32_t tag, uint32_t *tlen) {
    if (!d || len < 12) return nullptr;
    uint32_t v = ntf_be32(d);
    if (v != 0x00010000u && v != 0x74727565u) return nullptr;   // TrueType outlines only ('glyf')
    uint16_t nt = ntf_be16(d + 4);
    if (12 + (size_t)nt * 16 > len) return nullptr;
    for (uint16_t i = 0; i < nt; i++) {
        const uint8_t *rec = d + 12 + (size_t)i * 16;
        if (ntf_be32(rec) != tag) continue;
        uint32_t off = ntf_be32(rec + 8), tl = ntf_be32(rec + 12);
        if (!off || !tl || (size_t)off + tl < off || (size_t)off + tl > len) return nullptr;
        *tlen = tl;
        return d + off;
    }
    return nullptr;
}

// Family name: 'name' table, ID 16 (typographic) falling back to ID 1, Windows UTF-16BE
// (accepted only when every high byte is zero) or Macintosh Roman. Returns false when no
// representable name exists.
static bool ntf_sfnt_family(const uint8_t *d, size_t len, char out[NTF_FAMILY_MAX]) {
    uint32_t nlen = 0;
    const uint8_t *nm = ntf_sfnt_table(d, len, 0x6E616D65u, &nlen);   // 'name'
    if (!nm || nlen < 6) return false;
    uint16_t count = ntf_be16(nm + 2), stroff = ntf_be16(nm + 4);
    if (6 + (size_t)count * 12 > nlen) return false;
    static const uint16_t want_ids[2] = { 16, 1 };
    for (int w = 0; w < 2; w++) {
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t *rec = nm + 6 + (size_t)i * 12;
            uint16_t plat = ntf_be16(rec), nameid = ntf_be16(rec + 6);
            uint16_t slen = ntf_be16(rec + 8), soff = ntf_be16(rec + 10);
            if (nameid != want_ids[w]) continue;
            if ((size_t)stroff + soff + slen > nlen || slen == 0) continue;
            const uint8_t *s = nm + stroff + soff;
            char buf[NTF_FAMILY_MAX * 2];
            size_t bl = 0;
            if (plat == 3) {                       // Windows: UTF-16BE
                if (slen % 2) continue;
                bool ok = true;
                for (uint16_t j = 0; j + 1 < slen && bl + 1 < sizeof(buf); j += 2) {
                    if (s[j] != 0) { ok = false; break; }   // non-Latin name: not representable here
                    buf[bl++] = (char)s[j + 1];
                }
                if (!ok || bl == 0) continue;
            } else if (plat == 1) {                // Macintosh: byte encoding
                for (uint16_t j = 0; j < slen && bl + 1 < sizeof(buf); j++)
                    buf[bl++] = (char)s[j];
            } else {
                continue;
            }
            ntf_family_fold(buf, bl, out);
            if (out[0]) return true;
        }
    }
    return false;
}

// cmap format-4 lookup for a BMP code point. Returns 0 (missing glyph) on any inconsistency.
static uint16_t ntf_cmap4_lookup(const uint8_t *sub, uint32_t sublen, uint16_t c) {
    if (!sub || sublen < 14 || ntf_be16(sub) != 4) return 0;
    uint16_t segX2 = ntf_be16(sub + 6);
    if (segX2 < 2 || segX2 % 2 || 16u + 4u * segX2 > sublen) return 0;
    const uint8_t *endCodes = sub + 14;
    const uint8_t *startCodes = sub + 16 + segX2;
    const uint8_t *idDeltas = startCodes + segX2;
    const uint8_t *idRanges = idDeltas + segX2;
    for (uint16_t seg = 0; seg < segX2; seg += 2) {
        uint16_t end = ntf_be16(endCodes + seg);
        if (c > end) continue;
        uint16_t startc = ntf_be16(startCodes + seg);
        if (c < startc) return 0;
        uint16_t ro = ntf_be16(idRanges + seg);
        if (ro == 0)
            return (uint16_t)(c + ntf_be16(idDeltas + seg));   // mod 65536 by unsigned wrap
        size_t p = (size_t)(idRanges + seg - sub) + ro + 2u * (c - startc);
        if (p + 2 > sublen) return 0;
        uint16_t g = ntf_be16(sub + p);
        return g ? (uint16_t)(g + ntf_be16(idDeltas + seg)) : 0;
    }
    return 0;
}

// Parse one font file and fold its metrics into the family table: hhea ascent/descent for the
// baseline model, and the outline bounding boxes of a fixed repertoire (printable ASCII plus the
// Latin-1 letters) for the real ink extents. TrueType ('glyf') outlines only; anything else
// leaves the family unmeasured and the fix sits out for it. Never throws, never writes to the
// buffer, and a malformed font is simply skipped.
static void ntf_pagecut_register_font(const uint8_t *d, size_t len) {
    char fam[NTF_FAMILY_MAX];
    if (!ntf_sfnt_family(d, len, fam)) return;
    uint32_t headl = 0, hheal = 0, maxpl = 0, cmapl = 0, local = 0, glyfl = 0;
    const uint8_t *head = ntf_sfnt_table(d, len, 0x68656164u, &headl);   // 'head'
    const uint8_t *hhea = ntf_sfnt_table(d, len, 0x68686561u, &hheal);   // 'hhea'
    const uint8_t *maxp = ntf_sfnt_table(d, len, 0x6D617870u, &maxpl);   // 'maxp'
    const uint8_t *cmap = ntf_sfnt_table(d, len, 0x636D6170u, &cmapl);   // 'cmap'
    const uint8_t *loca = ntf_sfnt_table(d, len, 0x6C6F6361u, &local);   // 'loca'
    const uint8_t *glyf = ntf_sfnt_table(d, len, 0x676C7966u, &glyfl);   // 'glyf'
    if (!head || headl < 54 || !hhea || hheal < 8 || !maxp || maxpl < 6
        || !cmap || cmapl < 4 || !loca || !glyf)
        return;
    uint16_t upem = ntf_be16(head + 18);
    int16_t locfmt = (int16_t)ntf_be16(head + 50);
    int asc = (int16_t)ntf_be16(hhea + 4);
    int desc = -(int16_t)ntf_be16(hhea + 6);          // stored magnitude; typically negative in hhea
    uint16_t nglyphs = ntf_be16(maxp + 4);
    if (upem < 16 || asc <= 0 || desc < 0 || nglyphs == 0 || (locfmt != 0 && locfmt != 1))
        return;
    // Pick a usable format-4 cmap subtable: Windows BMP (3,1) first, then any Unicode (0,x).
    const uint8_t *sub = nullptr;
    uint32_t sublen = 0;
    uint16_t encs = ntf_be16(cmap + 2);
    if (4 + (size_t)encs * 8 > cmapl) return;
    for (int pass = 0; pass < 2 && !sub; pass++) {
        for (uint16_t i = 0; i < encs; i++) {
            const uint8_t *rec = cmap + 4 + (size_t)i * 8;
            uint16_t plat = ntf_be16(rec);
            uint32_t off = ntf_be32(rec + 4);
            if (pass == 0 ? !(plat == 3 && ntf_be16(rec + 2) == 1) : plat != 0) continue;
            if (off + 14 > cmapl) continue;
            if (ntf_be16(cmap + off) != 4) continue;
            sub = cmap + off;
            sublen = cmapl - off;
            break;
        }
    }
    if (!sub) return;
    // Ink extents over the repertoire. The glyf header bounding box (present for simple and
    // composite glyphs alike) is the unhinted outline extent — the same numbers an unscaled
    // FT_Load_Glyph would report.
    int inkAsc = 0, inkDesc = 0, measured = 0;
    for (unsigned c = 0x20; c <= 0xFF; c++) {
        if (c > 0x7E && c < 0xC0) continue;           // printable ASCII + Latin-1 letters
        uint16_t g = ntf_cmap4_lookup(sub, sublen, (uint16_t)c);
        if (g == 0 || g >= nglyphs) continue;
        size_t go, ge;
        if (locfmt == 0) {
            if ((size_t)2 * g + 4 > local) continue;
            go = (size_t)2 * ntf_be16(loca + 2 * g);
            ge = (size_t)2 * ntf_be16(loca + 2 * g + 2);
        } else {
            if ((size_t)4 * g + 8 > local) continue;
            go = ntf_be32(loca + 4 * g);
            ge = ntf_be32(loca + 4 * g + 4);
        }
        if (ge <= go || go + 10 > glyfl) continue;    // empty glyph (e.g. space) or out of range
        int ymin = (int16_t)ntf_be16(glyf + go + 4);
        int ymax = (int16_t)ntf_be16(glyf + go + 8);
        if (ymax > inkAsc) inkAsc = ymax;
        if (-ymin > inkDesc) inkDesc = -ymin;
        measured++;
    }
    // Require a real alphabet's worth of glyphs so a symbol font can't arm the fix with junk.
    if (measured < 26 || inkAsc <= 0 || inkDesc < 0) return;
    ntf_fontmetrics_t *m = ntf_fontmetrics_find(fam);
    if (!m) {
        if (ntf_fontmetrics_n >= NTF_FONTMETRICS_MAX) {
            static bool warned = false;
            if (!warned) { warned = true; NTF_DBG("pagecut fix: font-metrics table full; later families stay unmeasured"); }
            return;
        }
        m = &ntf_fontmetrics[ntf_fontmetrics_n++];
        memcpy(m->family, fam, sizeof(m->family));
        m->upem = upem;
        m->asc = asc;
        m->desc = desc;
        m->inkAsc = inkAsc;
        m->inkDesc = inkDesc;
        m->usable = true;
        NTF_DBG("pagecut fix: measured '%s' upem=%u asc=%d desc=%d inkAsc=%d inkDesc=%d",
            m->family, upem, asc, desc, inkAsc, inkDesc);
        return;
    }
    // Another face of a known family: ink maxima accumulate; a mismatched em size makes the
    // family unusable (the pixel conversion would be wrong for one of the faces).
    if (m->upem != upem) {
        if (m->usable) NTF_DBG("pagecut fix: '%s' has faces with different upem; leaving it unmeasured", m->family);
        m->usable = false;
        return;
    }
    if (inkAsc > m->inkAsc) m->inkAsc = inkAsc;
    if (inkDesc > m->inkDesc) m->inkDesc = inkDesc;
}

// --- active reading family, captured from the reader's own CSS injection ---
// The reader injects `* { font-family: <family> !important }` per chapter (the same injection
// Fixes 6 and 8 already ride). Publisher-default books inject no family; the fix then has no
// metrics to arm with and sits out, per the scope note above.
static void ntf_pagecut_capture_family(const QString &css) {
    int p = css.indexOf(QLatin1String("font-family"), 0, Qt::CaseInsensitive);
    if (p < 0) { ntf_pagecut_family[0] = '\0'; return; }
    int colon = css.indexOf(QLatin1Char(':'), p);
    if (colon < 0) { ntf_pagecut_family[0] = '\0'; return; }
    int vs = colon + 1, ve = vs;
    while (ve < css.size()) {
        QChar ch = css.at(ve);
        if (ch == QLatin1Char('!') || ch == QLatin1Char(';') || ch == QLatin1Char('}')) break;
        ve++;
    }
    while (vs < ve && css.at(vs).isSpace()) vs++;
    while (ve > vs && css.at(ve - 1).isSpace()) ve--;
    // Strip one layer of quotes (Fix 8 may have added them).
    if (ve - vs >= 2) {
        QChar q = css.at(vs);
        if ((q == QLatin1Char('"') || q == QLatin1Char('\'')) && css.at(ve - 1) == q) { vs++; ve--; }
    }
    QString value = css.mid(vs, ve - vs);
    // A fallback list or a generic keyword is not a measurable family; sit out for the book.
    if (value.isEmpty() || value.contains(QLatin1Char(',')) || ntf_css_generic_family(value)) {
        ntf_pagecut_family[0] = '\0';
        return;
    }
    QByteArray latin = value.toLatin1();
    ntf_family_fold(latin.constData(), (size_t)latin.size(), ntf_pagecut_family);
}

// --- the per-pass armed snapshot ---
// Armed by the WebkitView::locatePages hook when the pass belongs to the reader's view and the
// active family has measured metrics; disarmed when the pass returns. Pixel values are
// precomputed here so the per-cut clamp is pure integer comparison. GUI-thread only.
struct ntf_pagecut_arm_t {
    bool on;
    int ascPx;        // round(asc * S / upem): baseline offset below a conforming rect's top
    int inkAscPx;     // ceil(inkAsc * S / upem)
    int inkDescPx;    // ceil(inkDesc * S / upem)
    int hpredLo, hpredHi;   // conforming rect-height window (predicted height ± 2)
};
static ntf_pagecut_arm_t ntf_pagecut_arm;   // .on=false at start of day

static void ntf_pagecut_try_arm(void *self) {
    ntf_pagecut_arm.on = false;
    if (!ntf_pagecut_family[0] || !ntf_wv_fontSize) return;
    if (!(ntf_kepub_reader_view == self
          || (!ntf_kepub_reader_view && ntf_learn_reader_view(self))))
        return;                                       // not the reader's pagination: stay stock
    const ntf_fontmetrics_t *m = ntf_fontmetrics_find(ntf_pagecut_family);
    if (!m || !m->usable) return;
    int S = -1;
    try { S = ntf_wv_fontSize(self); } catch (...) { return; }
    // The stored reading-font-size, which for default-size body text is the rendered pixel size
    // (the reader zooms by S/15). The rect-height conformance check below re-validates this per
    // line, so a styled book that scales away simply makes every rect nonconforming.
    if (S < 8 || S > 300) return;
    long long u = m->upem;
    long long ascPx = ((long long)m->asc * S + u / 2) / u;
    long long descPx = ((long long)m->desc * S + u / 2) / u;
    long long slackPx = (135LL * S + 500) / 1000;     // the measured ~0.135 em rect slack
    long long inkAscPx = ((long long)m->inkAsc * S + u - 1) / u;
    long long inkDescPx = ((long long)m->inkDesc * S + u - 1) / u;
    long long hpred = ascPx + descPx + slackPx;
    if (ascPx <= 0 || inkAscPx <= 0 || hpred < 8 || hpred > 1000) return;
    ntf_pagecut_arm.ascPx = (int)ascPx;
    ntf_pagecut_arm.inkAscPx = (int)inkAscPx;
    ntf_pagecut_arm.inkDescPx = (int)inkDescPx;
    ntf_pagecut_arm.hpredLo = (int)(hpred - 2);
    ntf_pagecut_arm.hpredHi = (int)(hpred + 2);
    ntf_pagecut_arm.on = true;
}

// The clamp. `ret` is the stock cutPage return; the value handed back replaces it as the page
// boundary, so every exit path below must satisfy the caller's invariants (progress past
// `start`, never past `limit`) — returning `ret` itself always does, and an adjusted value is
// range-checked before it is used. Pure over its inputs plus the armed snapshot.
static int ntf_pagecut_clamp(const QVector<QRect> *rects, int start, int limit, int ret) {
    // Only a boundary that IS a run-rect bottom is modeled; the raw-limit fallback and anything
    // unrecognized stay stock (they are also what the probe reports, so a firmware where this
    // path drifts shows up in a log before it ever matters here).
    ntf_pagecut_cut_info ci = ntf_pagecut_classify(rects, start, limit, ret);
    if (ci.cls != NTF_CUT_EDGE) return ret;
    // Ambiguous boundary (two rects share this bottom at different tops — a mixed or
    // multi-column line): stock.
    for (int i = 0; i < ci.n; i++) {
        const QRect &r = rects->at(i);
        if ((long long)r.y() + r.height() == ci.best_end && r.y() != ci.best_top) return ret;
    }
    // The boundary line and the first line below it must both look like body text of the armed
    // font (height within ±2 of the prediction); headings, images and fallback-font lines leave
    // the boundary stock.
    if (ci.best_h < ntf_pagecut_arm.hpredLo || ci.best_h > ntf_pagecut_arm.hpredHi) return ret;
    long long next_top = 0, next_h = 0;
    bool have_next = false;
    for (int i = 0; i < ci.n; i++) {
        const QRect &r = rects->at(i);
        if ((long long)r.y() > ci.best_top && (!have_next || r.y() < next_top)) {
            have_next = true;
            next_top = r.y();
            next_h = r.height();
        }
    }
    if (!have_next) return ret;                       // last line of the chapter: nothing to protect
    if (next_h < ntf_pagecut_arm.hpredLo || next_h > ntf_pagecut_arm.hpredHi) return ret;
    long long adv = next_top - ci.best_top;
    if (adv <= 0 || adv > 3LL * ntf_pagecut_arm.hpredHi) return ret;   // not a normal line pair
    // The ink gap: below the boundary line's deepest ink, above the next line's tallest ink.
    long long gapLo = ci.best_top + ntf_pagecut_arm.ascPx + ntf_pagecut_arm.inkDescPx;
    long long gapHi = next_top + ntf_pagecut_arm.ascPx - ntf_pagecut_arm.inkAscPx;
    if (gapLo > gapHi) return ret;                    // lines too tight for a clean boundary: stock
    long long b = ret;
    if (b < gapLo) b = gapLo;
    if (b > gapHi) b = gapHi;
    if (b == ret) return ret;                         // healthy boundary: byte-identical pagination
    if (b <= start || b > limit) return ret;          // hard invariants (stall-loop guard)
    NTF_DBG("pagecut fix: moved boundary %d -> %lld (gap %lld..%lld, line top=%lld next=%lld)",
        ret, b, gapLo, gapHi, ci.best_top, next_top);
    return (int)b;
}

// addApplicationFont is static int(const QString&); via NickelHook it's a plain int(const QString*).
static int (*real_addApplicationFont)(const QString *) = nullptr;
// QRawFont::QRawFont(const QByteArray&, qreal, QFont::HintingPreference) — WebKit's embedded-font
// path. qreal is double here (the 'd' in the mangled name), passed in d0 under the hard-float ABI.
static void (*real_QRawFontCtor)(void *, const QByteArray *, double, int) = nullptr;

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
    //libnickel 4.21.15015 * _ZN13CustomWebView19setWritingDirectionE16WritingDirection
    // FIX 6 — reader-font fallback repair: the ctor resets per-book state; arm on the per-chapter
    // font-CSS injection, re-inject on the next page-set. All optional (a missing symbol just sits
    // the fix out).
    { .sym = "_ZN15KepubBookReaderC1EP11PluginStateP7QWidget", .sym_new = "_ntf_kepubReaderCtor",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kepubReaderCtor), .desc = "fix 6: reset per-book state", .optional = true },
    //libnickel 4.21.15015 * _ZN15KepubBookReaderC1EP11PluginStateP7QWidget
    { .sym = "_ZN15KepubBookReaderD1Ev", .sym_new = "_ntf_kepubReaderDtor",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kepubReaderDtor), .desc = "fix 6: clear destroyed reader state", .optional = true },
    //libnickel 4.21.15015 * _ZN15KepubBookReaderD1Ev
    { .sym = "_ZN10WebkitView12addCssToHtmlE7QString", .sym_new = "_ntf_wv_addCssToHtml",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_addCssToHtml), .desc = "arm reader-font re-apply", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView12addCssToHtmlE7QString
    { .sym = "_ZN10WebkitView14setCurrentPageEi", .sym_new = "_ntf_wv_setCurrentPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_setCurrentPage), .desc = "re-apply reader font per chapter", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView14setCurrentPageEi
    // (letter-spacing on spaces is an in-memory byte patch, not a hook — see NTF_JUSTIFY_FIXES.)
    // FIX 7 — capital spacing: strip cpsp from each reader font as it's registered. Optional; a
    // missing symbol just sits the fix out. QFontDatabase::addApplicationFont is a Qt import in
    // libnickel's PLT, hooked the same way as FT_Load_Glyph in libkobo. It carries no symbol
    // annotation on purpose: the symbol is imported, not defined here, so it has no offset in
    // libnickel, and test/syms (which resolves symbols to offsets) would report it missing on
    // every firmware.
    { .sym = "_ZN13QFontDatabase18addApplicationFontERK7QString", .sym_new = "_ntf_addApplicationFont",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_addApplicationFont), .desc = "fix 7: strip cpsp per font at load", .optional = true },
    // FIX 9, stage 1 — page-boundary probe. All optional; a missing symbol sits that part of the
    // probe out. cutPage and sortRectsByStart are static functions (no `this`); the two
    // locatePages hooks bracket each pagination pass so their lines can be attributed to a view.
    // All four are passthroughs: the probe never changes what the real functions return.
    { .sym = "_ZN10WebkitView7cutPageERK7QVectorI5QRectEii16WritingDirection", .sym_new = "_ntf_wv_cutPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_cutPage), .desc = "fix 9 probe: observe straddle page cuts", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView7cutPageERK7QVectorI5QRectEii16WritingDirection
    { .sym = "_ZN10WebkitView11locatePagesEb", .sym_new = "_ntf_wv_locatePages",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_locatePages), .desc = "fix 9 probe: bracket a pagination pass", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView11locatePagesEb
    { .sym = "_ZN19KepubBookReaderBase11locatePagesEb", .sym_new = "_ntf_kbrb_locatePages",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kbrb_locatePages), .desc = "fix 9 probe: mark annotation-path passes", .optional = true },
    //libnickel 4.21.15015 * _ZN19KepubBookReaderBase11locatePagesEb
    { .sym = "_ZN10WebkitView16sortRectsByStartER7QVectorI5QRectE16WritingDirection", .sym_new = "_ntf_wv_sortRects",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_wv_sortRects), .desc = "fix 9 probe: observe the pass's line rects", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView16sortRectsByStartER7QVectorI5QRectE16WritingDirection
    // FIX 7 (embedded fonts) — TO BE TESTED LATER. WebKit constructs a QRawFont from epub @font-face
    // bytes; this hooks that ctor in the WebKit lib's PLT to strip cpsp from embedded fonts too. Left
    // DISABLED (entry commented out) until it can be validated on-device with an epub whose @font-face
    // font actually carries cpsp — the reader-font hook above is confirmed, this path is not. The hook
    // body (_ntf_QRawFontCtor) and real_QRawFontCtor are kept below; re-enable by uncommenting this.
    // { .sym = "_ZN8QRawFontC1ERK10QByteArraydN5QFont17HintingPreferenceE", .sym_new = "_ntf_QRawFontCtor",
    //   .lib = "libQt5WebKit.so.5", .out = nh_symoutptr(real_QRawFontCtor), .desc = "fix 7: strip cpsp from embedded @font-face fonts", .optional = true },
    {0},
};
static struct nh_dlsym NickelTypeFixDlsym[] = {
    { .name = "_Z26writingDirectionFromStringRK7QString", .out = nh_symoutptr(ntf_writingDirectionFromString), .desc = "derive vertical enum ints", .optional = true },
    { .name = "_ZNK13CustomWebView8settingsEv", .out = nh_symoutptr(ntf_cwv_settings), .desc = "reach the page's QWebSettings", .optional = true },
    { .name = "_ZN12QWebSettings20setUserStyleSheetUrlERK4QUrl", .out = nh_symoutptr(ntf_setUserStyleSheetUrl), .desc = "set/clear the user stylesheet", .optional = true },
    { .name = "_ZNK12QWebSettings17userStyleSheetUrlEv", .out = nh_symoutptr(ntf_getUserStyleSheetUrl), .desc = "read the slot back before touching it", .optional = true },
    { .name = "_ZNK10WebkitView7webViewEv", .out = nh_symoutptr(ntf_wv_webView), .desc = "map a WebkitView to its CustomWebView", .optional = true },
    { .name = "_ZN15KepubBookReader12pageStyleCssEb", .out = nh_symoutptr(ntf_pageStyleCss), .desc = "fix 6: rebuild reader-font CSS", .optional = true },
    { .name = "_ZN15KepubBookReader12addCssToHtmlE7QString", .out = nh_symoutptr(ntf_kbr_addCssToHtml), .desc = "fix 6: re-inject reader-font CSS", .optional = true },
    // NOTE: an earlier revision resolved `_ZThn24_N15KepubBookReaderD1Ev` here and treated its
    // existence as proof that WebkitView is the +24 subobject. That thunk belongs to a different
    // base at +24; the view offset is learned per book instead (ntf_learn_reader_view).
    { .name = "_ZN10WebkitView8fontSizeEv", .out = nh_symoutptr(ntf_wv_fontSize), .desc = "fix 9 probe: log the reading font size per pass", .optional = true },
    //libnickel 4.21.15015 * _ZN10WebkitView8fontSizeEv
    { .name = "_ZNK10WebkitView10totalPagesEv", .out = nh_symoutptr(ntf_wv_totalPages), .desc = "fix 9 probe: log each pass's resulting page count", .optional = true },
    //libnickel 4.21.15015 * _ZNK10WebkitView10totalPagesEv
    { .name = "_ZNK10WebkitView13getPageOffsetEiRiS0_", .out = nh_symoutptr(ntf_wv_getPageOffset), .desc = "fix 9 probe: read back each placed page boundary", .optional = true },
    //libnickel 4.25.15875 * _ZNK10WebkitView13getPageOffsetEiRiS0_
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

// FIX 6 + FIX 2 — per-book reset. Clear the previous identities before calling Nickel, but do not
// publish the new reader until its real constructor has completed: a constructor-time WebkitView
// callback must not be able to re-enter Fix 6 with a partially constructed KepubBookReader.
extern "C" __attribute__((visibility("default")))
void _ntf_kepubReaderCtor(void *self, void *pluginState, void *widget) {
    bool on_qt = ntf_on_qt_thread();
    if (on_qt) {
        ntf_kepub_reader = nullptr;
        ntf_kepub_reader_view = nullptr;
        ntf_chapter_view = nullptr;
        ntf_chapter_needs_fix = false; // Fix 6: re-armed by each chapter's font-CSS injection
        ntf_fontfix_logged = false;    // let Fix 6 log its one friendly note again for this book
        ntf_vert_views_flush();        // Fix 2: stale per-view state must not survive into a new book
        ntf_pagecut_family[0] = '\0';  // Fix 9 stage 2: the new book announces its own family
        ntf_pagecut_arm.on = false;
    }
    // A NULL real constructor is unrecoverable (there is nothing to construct
    // with) but also unreachable: NickelHook only installs a hook whose symbol
    // resolved. The guard exists purely to avoid a jump through NULL.
    if (!real_kepubReaderCtor) return;
    real_kepubReaderCtor(self, pluginState, widget);
    if (on_qt) {
        ntf_kepub_reader = self;   // complete KepubBookReader; now safe for Fix 6 to call
        // The view starts unknown for every book and is learned (with an ABI
        // proof) from the first font-CSS injection — ntf_learn_reader_view.
        // Assuming an offset here is exactly the v0.5 mistake that disarmed
        // this fix; see the history note at the state block.
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
        ntf_pagecut_family[0] = '\0';   // Fix 9 stage 2: no reader, no active family
        ntf_pagecut_arm.on = false;
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

// ================= FIX 8: reader-font quoting (libnickel / injected CSS) =================
// Kepub reader-font CSS is injected from the unquoted template `* { font-family: %1 !important; }`
// (KepubBookReader::pageStyleCss substitutes the raw family into %1). If the family name has a
// whitespace-separated token that starts with a digit (e.g. "Roboto 2", "Helvetica 75", "Bitter 24pt"),
// the emitted `font-family: Roboto 2 !important` is invalid CSS (an unquoted identifier can't start
// with a digit), so WebKit drops the declaration and the reader falls back to its default font. That
// this is a quoting oversight is visible in the same binary, which hard-codes a QUOTED sibling rule
// `rt { font-family: 'Sans-SerifJP' !important; }`. The font is registered in QFontDatabase under its
// true family, so the fix is simply to quote the family in the injected rule. Applied to the same
// addCssToHtml `css` copy the other CSS fixes use, before the real call.
static bool ntf_quote_fontfamily() { return ntf_global_config_bool("ntf_quote_fontfamily", true); }

// True if `value` is a bare CSS generic family keyword. A generic must NOT be quoted: quoting turns it
// into a (non-existent) family-name lookup, which would break it. Case-insensitive; the -webkit- prefix
// covers WebKit's vendor generics (e.g. -webkit-body).
static bool ntf_css_generic_family(const QString &value) {
    static const char *const generics[] = {
        "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", NULL,
    };
    for (int i = 0; generics[i]; i++)
        if (value.compare(QLatin1String(generics[i]), Qt::CaseInsensitive) == 0) return true;
    return value.startsWith(QLatin1String("-webkit-"), Qt::CaseInsensitive);
}

// Wrap the value of every `font-family: <value> !important` declaration in `css` in double quotes,
// where <value> is a single unquoted family name. The reader rule has exactly this shape (value
// terminated by !important); a value terminated by ';' or '}' before any !important is a different
// declaration and is left alone. Skips a value that is already quoted (leaves 'Sans-SerifJP' intact),
// a comma-separated fallback list, or a bare generic keyword. Only ever inserts two quote chars around
// an unquoted value, so re-running on its own output is a no-op (the already-quoted skip makes it
// idempotent). A real family name never contains a double-quote char, so double quotes are safe.
static void ntf_quote_reader_fontfamily(QString &css) {
    const QString prop = QLatin1String("font-family:");
    const QString bang = QLatin1String("!important");
    int from = 0;
    while (true) {
        int p = css.indexOf(prop, from, Qt::CaseInsensitive);
        if (p < 0) break;
        int vstart = p + prop.length();
        int bpos = css.indexOf(bang, vstart, Qt::CaseInsensitive);
        if (bpos < 0) break;                                   // no !important-terminated value left
        // Only treat this as the reader rule if nothing ends the declaration before !important.
        bool terminated = false;
        for (int i = vstart; i < bpos; i++) {
            QChar c = css.at(i);
            if (c == QLatin1Char(';') || c == QLatin1Char('}')) { terminated = true; break; }
        }
        if (terminated) { from = vstart; continue; }
        int vs = vstart, ve = bpos;                            // trim to the value between colon and !important
        while (vs < ve && css.at(vs).isSpace()) vs++;
        while (ve > vs && css.at(ve - 1).isSpace()) ve--;
        if (vs >= ve) { from = bpos + bang.length(); continue; }   // empty value
        QChar first = css.at(vs);
        QString value = css.mid(vs, ve - vs);
        bool skip = first == QLatin1Char('\'') || first == QLatin1Char('"')   // already quoted
                    || value.contains(QLatin1Char(','))                       // fallback list
                    || ntf_css_generic_family(value);                         // bare generic keyword
        if (skip) { from = bpos + bang.length(); continue; }
        css.insert(ve, QLatin1Char('"'));
        css.insert(vs, QLatin1Char('"'));
        from = bpos + 2 + bang.length();                       // two quotes added before !important
    }
}

// FIX 6 — arm the per-chapter re-inject. WebkitView::addCssToHtml is called when a chapter injects its
// font CSS (once per chapter load; not on plain page turns), which is our per-chapter, font-agnostic
// "a fresh chapter drew" signal. Our own re-inject also calls this (via KepubBookReader::addCssToHtml),
// so ntf_in_fixonturn suppresses re-arming to avoid a loop. `css` is passed by hidden reference; we
// only read state, never mutate it.
extern "C" __attribute__((visibility("default")))
void _ntf_wv_addCssToHtml(void *self, QString *css) {
    try {
        // WebkitView is shared by dictionary/store/browser views.  Only a call on
        // the current KepubBookReader may arm Fix 6; otherwise a later page change
        // could route a non-reader event into the reader-font methods. The
        // reader's view starts out unknown for every book, and the first
        // injection while a reader is live learns it with an ABI proof
        // (ntf_learn_reader_view) — offset 0 included.
        if (ntf_enabled() && ntf_kepub_fontfix() && real_kepubReaderDtor && !ntf_in_fixonturn
            && ntf_on_qt_thread()
            && (ntf_kepub_reader_view == self
                || (!ntf_kepub_reader_view && ntf_learn_reader_view(self)))) {
            ntf_chapter_needs_fix = true;
            ntf_chapter_view = self;
        }
        // FIX 9, stage 2: remember the reading family this book injects, so the boundary fix
        // can look up the measured metrics when the reader paginates. Same reader-view identity
        // proof as Fix 6 above, but under the fix's own key so the two stay independent.
        if (ntf_enabled() && ntf_pagecut_fix() && css && ntf_on_qt_thread()
            && (ntf_kepub_reader_view == self
                || (!ntf_kepub_reader_view && ntf_learn_reader_view(self)))) {
            ntf_pagecut_capture_family(*css);
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
        // FIX 8: quote the injected reader-font family so a digit-token name (e.g. "Roboto 2") stays
        // valid CSS instead of being dropped. Runs last so it composes with the fixes above: it only
        // touches font-family declarations, leaving Fix 2's appended text-rendering rule alone, and a
        // non-matching or already-quoted `css` passes through byte-for-byte.
        if (ntf_enabled() && ntf_quote_fontfamily() && css) {
            ntf_quote_reader_fontfamily(*css);
        }
    } catch (...) {
        // Contain Qt allocation failures (see _ntf_cwv_setWritingDirection);
        // the injection then goes through unmodified, which is stock behavior.
        NTF_LOG("Note: a CSS-injection fix skipped one update after an internal error (likely low memory).");
    }
    if (real_wv_addCssToHtml) real_wv_addCssToHtml(self, css);
}

// FIX 6 — consume: on the first setCurrentPage after a chapter drew (armed above), re-apply the
// reader-font CSS into the live document. If the chapter rendered its text in a substitute because the
// font was not ready in time, this re-resolves it in place; if the chapter is already correct the
// re-apply renders the identical font and is invisible.
extern "C" __attribute__((visibility("default")))
void _ntf_wv_setCurrentPage(void *self, int page) {
    if (real_wv_setCurrentPage) real_wv_setCurrentPage(self, page);
    // Consume only on the same live reader/view that armed the flag.  The
    // destructor normally clears ntf_kepub_reader; the identity checks also
    // make a missing destructor hook fail safe by sitting Fix 6 out.
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
    // The file is read when either consumer wants it: Fix 7 to strip cpsp, Fix 9 stage 2 to
    // measure the family's metrics. The metrics pass reads the ORIGINAL bytes and never edits.
    // No thread guard here (this hook must not become the guard's first claimant): registration
    // happens at boot and on library rescans, when no reader is paginating, and each metrics
    // entry publishes its `usable` flag last, so a reader thread never trusts a half-written
    // record.
    bool want_cpsp = ntf_enabled() && ntf_cpsp_fix();
    bool want_metrics = ntf_enabled() && ntf_pagecut_fix();
    if ((!want_cpsp && !want_metrics) || !fileName) return real_addApplicationFont(fileName);
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
        if (want_metrics)
            ntf_pagecut_register_font(reinterpret_cast<const uint8_t *>(buf.constData()), (size_t)sz);
        if (!want_cpsp) return real_addApplicationFont(fileName);
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

// FIX 9, stage 1 — page-boundary probe. With only the probe on, all four hooks are strict
// passthroughs: the real function always runs (first, so the returned value exists before any
// probe logic) and its result is handed back unchanged, so pagination is byte-identical to
// stock. The one thing that can change a return value is stage 2's clamp in the cutPage hook,
// under its own key (ntf_pagecut_fix) and armed state — never the probe.
// The probe logic itself only reads the hook's arguments and writes log lines;
// formatting is plain snprintf (no Qt allocation), and the try/catch matches the house rule
// that nothing may unwind out of an extern "C" hook. Nothing is ever dropped silently: an event
// the GUI-thread pass bracket cannot own becomes a capped "stray" line (see
// ntf_pagecut_stray_ok), and the very first cutPage call of a boot logs unconditionally, so a
// log with passes but no cut lines is positive evidence that cutPage did not run.
extern "C" __attribute__((visibility("default")))
int _ntf_wv_cutPage(const QVector<QRect> *rects, int start, int limit, int dir) {
    int ret = real_wv_cutPage(rects, start, limit, dir);
    // FIX 9, stage 2 — the boundary clamp. Runs only when the enclosing reader pass armed it
    // (its own key, ntf_pagecut_fix) and only for horizontal passes; every unprovable case
    // inside returns the stock value. The probe below deliberately observes the STOCK return,
    // so its cut lines keep describing the firmware's own decision; the fix logs its own DBG
    // line per moved boundary, and the pass-end boundary lines read the finished (clamped)
    // page table.
    int out = ret;
    if (ntf_enabled() && ntf_pagecut_arm.on && dir == 0 && ntf_on_qt_thread()) {
        try {
            out = ntf_pagecut_clamp(rects, start, limit, ret);
        } catch (...) {
            out = ret;
            NTF_LOG("Note: the page-boundary fix skipped one cut after an internal error.");
        }
    }
    if (!ntf_enabled() || !ntf_pagecut_probe()) return out;
    try {
        if (!__atomic_exchange_n(&ntf_pagecut_cut_seen, true, __ATOMIC_RELAXED))
            NTF_LOG("pagecut probe: first cutPage call of this boot (tid=%lx)", (unsigned long)pthread_self());
        if (ntf_on_qt_thread() && ntf_pagecut_depth > 0) {
            ntf_pagecut_observe(rects, start, limit, dir, ret);
        } else if (ntf_pagecut_stray_ok()) {
            // Off the claimed thread, or no locatePages frame open. The classification is pure
            // over the arguments and the vector belongs to the caller's own stack frame, so
            // this is safe on any thread; ntf_pagecut_depth is read only to describe the
            // anomaly in the line.
            ntf_pagecut_cut_info ci = ntf_pagecut_classify(rects, start, limit, ret);
            NTF_LOG("pagecut probe: stray cut (tid=%lx depth=%d): start=%d limit=%d dir=%d n=%d ret=%d cls=%s",
                (unsigned long)pthread_self(), ntf_pagecut_depth, start, limit, dir, ci.n, ret,
                ntf_pagecut_cls_name[ci.cls]);
        }
    } catch (...) {
        NTF_LOG("Note: the page-boundary probe skipped one observation after an internal error.");
    }
    return out;
}
extern "C" __attribute__((visibility("default")))
void *_ntf_wv_locatePages(void *self, int reload) {
    // pass_enter/pass_leave contain their Qt calls internally and otherwise only touch plain
    // ints and snprintf, so they cannot unwind; the enter/leave pairing per frame is what keeps
    // the pass bracket balanced (`probing` is computed once so a config change mid-call cannot
    // unbalance it).
    bool probing = ntf_enabled() && ntf_pagecut_probe();
    bool on_gui = probing && ntf_on_qt_thread();
    if (probing && !on_gui && ntf_pagecut_stray_ok())
        NTF_LOG("pagecut probe: stray locatePages (tid=%lx): view=%p reload=%d",
            (unsigned long)pthread_self(), self, reload);
    if (on_gui) ntf_pagecut_pass_enter(self, reload, false);
    // FIX 9, stage 2: snapshot the boundary-fix context for this pass (reader identity, family
    // metrics, font size), so the cutPage clamp below works from one consistent set of numbers.
    // Independent of the probe gates above; disarmed on the way out so nothing leaks between
    // passes or views.
    bool fixing = ntf_enabled() && ntf_pagecut_fix() && ntf_on_qt_thread();
    if (fixing) try { ntf_pagecut_try_arm(self); } catch (...) { ntf_pagecut_arm.on = false; }
    void *ret = real_wv_locatePages(self, reload);
    if (fixing) ntf_pagecut_arm.on = false;
    if (on_gui) ntf_pagecut_pass_leave(self);
    return ret;
}
extern "C" __attribute__((visibility("default")))
void *_ntf_kbrb_locatePages(void *self, int reload) {
    bool probing = ntf_enabled() && ntf_pagecut_probe();
    bool on_gui = probing && ntf_on_qt_thread();
    if (probing && !on_gui && ntf_pagecut_stray_ok())
        NTF_LOG("pagecut probe: stray reader locatePages (tid=%lx): view=%p reload=%d",
            (unsigned long)pthread_self(), self, reload);
    if (on_gui) ntf_pagecut_pass_enter(self, reload, true);
    void *ret = real_kbrb_locatePages(self, reload);
    if (on_gui) ntf_pagecut_pass_leave(self);
    return ret;
}
// sortRectsByStart is the "this pass did real pagination work" marker: its one caller sits past
// locatePages' early exits and right before the page walk, and the vector it just sorted is the
// very vector any cutPage call of the pass will receive. Logged once per pass; the real sort has
// already run when the probe reads the vector, so the summary shows the walk's actual input.
extern "C" __attribute__((visibility("default")))
void *_ntf_wv_sortRects(QVector<QRect> *rects, int dir) {
    void *ret = real_wv_sortRects(rects, dir);
    if (!ntf_enabled() || !ntf_pagecut_probe()) return ret;
    try {
        if (ntf_on_qt_thread() && ntf_pagecut_depth > 0) {
            if (ntf_pagecut_sorted++ == 0) {
                char buf[192];
                ntf_pagecut_fmt_rects(rects, buf, sizeof(buf));
                NTF_LOG("pagecut probe: pass %u sorted n=%d dir=%d %s",
                    ntf_pagecut_pass, rects ? rects->size() : -1, dir, buf);
                // Cache the geometry for the pass-end boundary lines. Plain int copies (no Qt
                // state kept); the end is computed in 64-bit and clamped because the rect
                // fields are untrusted.
                int n = rects ? rects->size() : 0;
                ntf_pagecut_rect_trunc = n > NTF_PAGECUT_RECTS_MAX;
                ntf_pagecut_rect_n = ntf_pagecut_rect_trunc ? NTF_PAGECUT_RECTS_MAX : n;
                ntf_pagecut_rect_dir = dir;
                for (int i = 0; i < ntf_pagecut_rect_n; i++) {
                    const QRect &r = rects->at(i);
                    long long e = (long long)r.y() + r.height();
                    if (e > INT32_MAX) e = INT32_MAX;
                    if (e < INT32_MIN) e = INT32_MIN;
                    ntf_pagecut_rect_tops[i] = r.y();
                    ntf_pagecut_rect_ends[i] = (int)e;
                }
            }
        } else if (ntf_pagecut_stray_ok()) {
            NTF_LOG("pagecut probe: stray sortRects (tid=%lx depth=%d): n=%d dir=%d",
                (unsigned long)pthread_self(), ntf_pagecut_depth, rects ? rects->size() : -1, dir);
        }
    } catch (...) {
        NTF_LOG("Note: the page-boundary probe skipped one observation after an internal error.");
    }
    return ret;
}

// FIX 7 (embedded fonts) — TO BE TESTED LATER; NOT CURRENTLY WIRED (its entry in NickelTypeFixHooks
// is commented out until it can be validated on-device). Kept here ready to re-enable.
// kepub @font-face fonts don't go through addApplicationFont; WebKit builds
// them straight into a QRawFont from the embedded bytes. Same treatment, done in place: strip cpsp
// from the QByteArray before the ctor parses it. WebKit owns fontData and keeps it alive across the
// ctor, so there's no lifetime concern; a shared buffer detaches (copies) before we touch it, so
// another holder of the same bytes is unaffected. Best-effort: on any problem the ctor runs on the
// original data. The try/catch contains Qt allocation failures (a throw out of an extern "C" hook
// would std::terminate Nickel).
extern "C" __attribute__((visibility("default")))
void _ntf_QRawFontCtor(void *self, const QByteArray *fontData, double pixelSize, int hintingPreference) {
    if (!real_QRawFontCtor) return;
    if (ntf_enabled() && ntf_cpsp_fix() && fontData && !fontData->isEmpty()) {
        try {
            QByteArray *mut = const_cast<QByteArray *>(fontData);
            if (ntf_strip_cpsp(reinterpret_cast<uint8_t *>(mut->data()), (size_t)mut->size()))
                NTF_DBG("cpsp: stripped Capital Spacing from an embedded font (%d bytes)", mut->size());
        } catch (...) {
            NTF_LOG("Note: the capital-spacing fix skipped one embedded font after an internal error (likely low memory).");
        }
    }
    real_QRawFontCtor(self, fontData, pixelSize, hintingPreference);
}
