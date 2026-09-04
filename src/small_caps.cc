// FIX 14 — real small caps (libQtGui / QTextEngine)
//
// A book that asks for `font-variant: small-caps` gets shrunken capitals on this reader, whatever
// the font carries. WebKit's Qt port hands the run to QTextLayout with QFont::SmallCaps set, and
// Qt then does two things to every lowercase stretch: it uppercases the text before shaping
// (QTextEngine::shapeText) and it swaps in a font engine cloned at 70% of the pixel size
// (QTextEngine::fontEngine, smallCapsFraction). HarfBuzz is only ever asked for `kern`, and the
// Qt port never reads a stylesheet's font-feature-settings, so a font's own `smcp` glyphs are
// loaded and ignored. The result is the right height and the wrong weight: thin capitals that sit
// badly beside the lowercase around them.
//
// Two seams undo that, and both are needed.
//
// QTextEngine::fontEngine is exported. Its detour asks the original for the engine of the same
// item with the small caps flag cleared, which is the full-size engine, and returns that when the
// font has an `smcp` feature. Every caller then works at the real size: the shaper, the metrics,
// and the draw. A font without `smcp` gets the stock scaled engine and nothing changes for it.
//
// The shaper detour that fix 12 installs sees the uppercased copy of the text, but the original
// lowercase text is still in the engine's layout data at the item's position. For a small caps
// item it shapes that instead, then substitutes each glyph through the font's own `smcp` single
// substitution table, recomputes the advances from the engine, and re-applies the font's `kern`
// pair adjustments between the new glyphs. Ligatures the shaper formed in the lowercase text
// (office, affix) are opened back up from the font's `liga` table first, since a ligature has no
// small cap form. All of it is read from the font's GSUB and GPOS tables through the engine's own
// sfnt access, so no file is opened and no family name is matched.
//
// This is not HarfBuzz. It handles what small caps in a book are: Latin runs of letters, spaces
// and punctuation, with precomposed accents. A run in a right-to-left context, a glyph from a
// fallback font, or a glyph the font has no small cap for is left as the shaper produced it.
//
// Only the complex path reaches a shaper, so this needs `optimizeLegibility` like fixes 3, 7 and
// 12. On the simple path WebCore shrinks the capitals itself and nothing here runs.
//
// Everything fails closed. If the fontEngine prologue cannot be detoured, the fix sits out and
// the reader keeps its shrunken capitals. A malformed table reads as "no small caps".

#define private public
#define protected public
#include <QtGui/private/qtextengine_p.h>
#undef private
#undef protected
#include <QtGui/private/qfontengine_p.h>
#include <QtCore/QByteArray>
#include <QtCore/QVector>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <stdint.h>

#include "small_caps.h"

extern "C" int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out);

static ntf_smallcaps_logger ntf_sc_logger = 0;
static bool ntf_sc_active = false;

static void ntf_sc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ntf_sc_log(const char *fmt, ...)
{
    if (!ntf_sc_logger) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ntf_sc_logger(line);
}

// ---- OpenType table reading ------------------------------------------------------------------
// Every read is bounds-checked against the table. A read past the end returns zero, and every
// loop that walks a count checks the whole array fits first, so a corrupt table cannot walk out
// of the buffer; it can only fail to yield a substitution.

#define NTF_TAG(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) << 24 | (uint32_t)(uint8_t)(b) << 16 | (uint32_t)(uint8_t)(c) << 8 | (uint32_t)(uint8_t)(d))

struct NtfTable {
    const uint8_t *p;
    uint32_t n;
    bool ok(uint32_t off, uint32_t len) const { return off <= n && len <= n - off; }
    uint16_t u16(uint32_t off) const { return ok(off, 2) ? (uint16_t)((p[off] << 8) | p[off + 1]) : 0; }
    int16_t s16(uint32_t off) const { return (int16_t)u16(off); }
    uint32_t u32(uint32_t off) const {
        return ok(off, 4) ? ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16)
                          | ((uint32_t)p[off + 2] << 8) | p[off + 3] : 0;
    }
};

// Index of a glyph in a Coverage table, or -1.
static int ntf_coverage_index(const NtfTable &t, uint32_t off, uint16_t g)
{
    const uint16_t format = t.u16(off);
    if (format == 1) {
        const uint16_t count = t.u16(off + 2);
        if (!t.ok(off + 4, (uint32_t)count * 2)) return -1;
        int lo = 0, hi = (int)count - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const uint16_t v = t.u16(off + 4 + (uint32_t)mid * 2);
            if (v == g) return mid;
            if (v < g) lo = mid + 1; else hi = mid - 1;
        }
        return -1;
    }
    if (format == 2) {
        const uint16_t count = t.u16(off + 2);
        if (!t.ok(off + 4, (uint32_t)count * 6)) return -1;
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t r = off + 4 + (uint32_t)i * 6;
            const uint16_t start = t.u16(r), end = t.u16(r + 2);
            if (g < start) return -1;
            if (g <= end) return (int)t.u16(r + 4) + (g - start);
        }
    }
    return -1;
}

// Every (glyph, coverage index) pair a Coverage table lists.
template <typename F>
static void ntf_coverage_each(const NtfTable &t, uint32_t off, F fn)
{
    const uint16_t format = t.u16(off);
    if (format == 1) {
        const uint16_t count = t.u16(off + 2);
        if (!t.ok(off + 4, (uint32_t)count * 2)) return;
        for (uint16_t i = 0; i < count; ++i) fn(t.u16(off + 4 + (uint32_t)i * 2), (int)i);
    } else if (format == 2) {
        const uint16_t count = t.u16(off + 2);
        if (!t.ok(off + 4, (uint32_t)count * 6)) return;
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t r = off + 4 + (uint32_t)i * 6;
            const uint16_t start = t.u16(r), end = t.u16(r + 2), ci = t.u16(r + 4);
            if (end < start || end - start > 4096) return;   // a range that size is not a real font
            for (uint32_t g = start; g <= end; ++g) fn((uint16_t)g, (int)ci + (int)(g - start));
        }
    }
}

// Class of a glyph in a ClassDef table; 0 when unlisted, as the spec says.
static uint16_t ntf_class_of(const NtfTable &t, uint32_t off, uint16_t g)
{
    const uint16_t format = t.u16(off);
    if (format == 1) {
        const uint16_t start = t.u16(off + 2), count = t.u16(off + 4);
        if (g < start || g >= start + count || !t.ok(off + 6, (uint32_t)count * 2)) return 0;
        return t.u16(off + 6 + (uint32_t)(g - start) * 2);
    }
    if (format == 2) {
        const uint16_t count = t.u16(off + 2);
        if (!t.ok(off + 4, (uint32_t)count * 6)) return 0;
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t r = off + 4 + (uint32_t)i * 6;
            const uint16_t start = t.u16(r), end = t.u16(r + 2);
            if (g < start) return 0;
            if (g <= end) return t.u16(r + 4);
        }
    }
    return 0;
}

// The lookup indices a feature with this tag lists, from any script. GSUB and GPOS share the
// header layout: version(4) scriptList(2) featureList(2) lookupList(2).
static void ntf_feature_lookups(const NtfTable &t, uint32_t tag, QVector<uint16_t> &out)
{
    if (t.u16(0) != 1) return;                          // major version 1 only
    const uint32_t fl = t.u16(6);
    const uint16_t count = t.u16(fl);
    if (!fl || !t.ok(fl + 2, (uint32_t)count * 6)) return;
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t rec = fl + 2 + (uint32_t)i * 6;
        if (t.u32(rec) != tag) continue;
        const uint32_t feat = fl + t.u16(rec + 4);
        const uint16_t lc = t.u16(feat + 2);
        if (!t.ok(feat + 4, (uint32_t)lc * 2)) continue;
        for (uint16_t j = 0; j < lc; ++j) out.append(t.u16(feat + 4 + (uint32_t)j * 2));
    }
}

// The subtables of one lookup that are of the wanted type, seen through Extension lookups.
static void ntf_lookup_subtables(const NtfTable &t, uint16_t index, uint16_t want, uint16_t ext,
                                 QVector<uint32_t> &out)
{
    const uint32_t ll = t.u16(8);
    const uint16_t count = t.u16(ll);
    if (!ll || index >= count || !t.ok(ll + 2, (uint32_t)count * 2)) return;
    const uint32_t lookup = ll + t.u16(ll + 2 + (uint32_t)index * 2);
    const uint16_t type = t.u16(lookup), subs = t.u16(lookup + 4);
    if (!t.ok(lookup + 6, (uint32_t)subs * 2)) return;
    for (uint16_t i = 0; i < subs; ++i) {
        const uint32_t sub = lookup + t.u16(lookup + 6 + (uint32_t)i * 2);
        if (type == want) {
            out.append(sub);
        } else if (type == ext && t.u16(sub) == 1 && t.u16(sub + 2) == want) {
            const uint32_t inner = sub + t.u32(sub + 4);
            if (t.ok(inner, 2)) out.append(inner);
        }
    }
}

// ---- what the fix knows about one font ---------------------------------------------------------

struct NtfScLigature {
    uint16_t ligature;
    uint8_t count;            // components, 2 to 4
    uint16_t components[4];
};

struct NtfScFont {
    unsigned long id;
    bool has_smcp;
    uint16_t upem;
    uint16_t num_glyphs;
    QByteArray gsub;          // kept only while it is being read
    QByteArray gpos;          // kept: kerning is evaluated per pair
    uint16_t *smcp;           // num_glyphs entries; 0 means no small cap form
    QVector<NtfScLigature> ligatures;
    QVector<uint32_t> kern_subtables;   // PairPos subtables of the `kern` feature, in order
};

static const int NTF_SC_MAX_FONTS = 32;
static NtfScFont *ntf_sc_fonts[NTF_SC_MAX_FONTS];
static int ntf_sc_font_count = 0;
static pthread_mutex_t ntf_sc_lock = PTHREAD_MUTEX_INITIALIZER;

// The face, not the size: two engines for the same face at different pixel sizes read the same
// tables, and a scaled clone must resolve to the same record as the engine it was cloned from.
static unsigned long ntf_sc_face_identity(const QFontEngine *fe)
{
    unsigned long h = 2166136261UL;
    #define NTF_SC_MIX_BYTES(ptr, len) do { \
        const unsigned char *b = (const unsigned char *)(ptr); \
        for (size_t i = 0; i < (size_t)(len); ++i) { h ^= b[i]; h *= 16777619UL; } \
    } while (0)
    #define NTF_SC_MIX(v) do { typeof(v) tmp_ = (v); NTF_SC_MIX_BYTES(&tmp_, sizeof tmp_); } while (0)
    const QFontDef &d = fe->fontDef;
    const QByteArray fam = d.family.toUtf8();
    NTF_SC_MIX_BYTES(fam.constData(), fam.size());
    const QByteArray sty = d.styleName.toUtf8();
    NTF_SC_MIX_BYTES(sty.constData(), sty.size());
    NTF_SC_MIX(d.weight);
    NTF_SC_MIX(d.style);
    NTF_SC_MIX(d.stretch);
    NTF_SC_MIX(d.fixedPitch);
    const int t = (int)fe->type();
    NTF_SC_MIX(t);
    #undef NTF_SC_MIX
    #undef NTF_SC_MIX_BYTES
    return h ? h : 1;
}

static void ntf_sc_read_smcp(NtfScFont *f)
{
    const NtfTable t = { (const uint8_t *)f->gsub.constData(), (uint32_t)f->gsub.size() };
    QVector<uint16_t> lookups;
    ntf_feature_lookups(t, NTF_TAG('s', 'm', 'c', 'p'), lookups);
    if (lookups.isEmpty()) return;
    QVector<uint32_t> subs;
    for (int i = 0; i < lookups.size(); ++i) ntf_lookup_subtables(t, lookups[i], 1, 7, subs);
    if (subs.isEmpty()) return;

    f->smcp = (uint16_t *)calloc(f->num_glyphs, sizeof(uint16_t));
    if (!f->smcp) return;
    uint16_t *map = f->smcp;
    const uint16_t num_glyphs = f->num_glyphs;
    int mapped = 0;
    for (int i = 0; i < subs.size(); ++i) {
        const uint32_t sub = subs[i];
        const uint16_t format = t.u16(sub);
        const uint32_t coverage = sub + t.u16(sub + 2);
        if (format == 1) {
            const int16_t delta = t.s16(sub + 4);
            ntf_coverage_each(t, coverage, [&](uint16_t g, int) {
                const uint16_t to = (uint16_t)(g + delta);
                if (g < num_glyphs && to < num_glyphs && !map[g]) { map[g] = to; mapped++; }
            });
        } else if (format == 2) {
            const uint16_t count = t.u16(sub + 4);
            if (!t.ok(sub + 6, (uint32_t)count * 2)) continue;
            ntf_coverage_each(t, coverage, [&](uint16_t g, int ci) {
                if (ci < 0 || ci >= (int)count) return;
                const uint16_t to = t.u16(sub + 6 + (uint32_t)ci * 2);
                if (g < num_glyphs && to < num_glyphs && !map[g]) { map[g] = to; mapped++; }
            });
        }
    }
    f->has_smcp = mapped > 0;
}

// Ligatures the shaper may form in lowercase text, so they can be opened back up. `liga` and
// `clig` are what HarfBuzz applies by default for Latin.
static void ntf_sc_read_ligatures(NtfScFont *f)
{
    const NtfTable t = { (const uint8_t *)f->gsub.constData(), (uint32_t)f->gsub.size() };
    QVector<uint16_t> lookups;
    ntf_feature_lookups(t, NTF_TAG('l', 'i', 'g', 'a'), lookups);
    ntf_feature_lookups(t, NTF_TAG('c', 'l', 'i', 'g'), lookups);
    QVector<uint32_t> subs;
    for (int i = 0; i < lookups.size(); ++i) ntf_lookup_subtables(t, lookups[i], 4, 7, subs);
    for (int i = 0; i < subs.size(); ++i) {
        const uint32_t sub = subs[i];
        if (t.u16(sub) != 1) continue;
        const uint32_t coverage = sub + t.u16(sub + 2);
        const uint16_t set_count = t.u16(sub + 4);
        if (!t.ok(sub + 6, (uint32_t)set_count * 2)) continue;
        ntf_coverage_each(t, coverage, [&](uint16_t first, int ci) {
            if (ci < 0 || ci >= (int)set_count) return;
            const uint32_t set = sub + t.u16(sub + 6 + (uint32_t)ci * 2);
            const uint16_t lig_count = t.u16(set);
            if (!t.ok(set + 2, (uint32_t)lig_count * 2)) return;
            for (uint16_t j = 0; j < lig_count; ++j) {
                const uint32_t lig = set + t.u16(set + 2 + (uint32_t)j * 2);
                const uint16_t glyph = t.u16(lig), comps = t.u16(lig + 2);
                if (comps < 2 || comps > 4 || !t.ok(lig + 4, (uint32_t)(comps - 1) * 2)) continue;
                NtfScLigature l;
                l.ligature = glyph;
                l.count = (uint8_t)comps;
                l.components[0] = first;
                for (uint16_t k = 1; k < comps; ++k) l.components[k] = t.u16(lig + 4 + (uint32_t)(k - 1) * 2);
                if (f->ligatures.size() < 256) f->ligatures.append(l);
            }
        });
    }
}

static void ntf_sc_read_kerning(NtfScFont *f)
{
    const NtfTable t = { (const uint8_t *)f->gpos.constData(), (uint32_t)f->gpos.size() };
    QVector<uint16_t> lookups;
    ntf_feature_lookups(t, NTF_TAG('k', 'e', 'r', 'n'), lookups);
    for (int i = 0; i < lookups.size(); ++i) ntf_lookup_subtables(t, lookups[i], 2, 9, f->kern_subtables);
}

static inline uint32_t ntf_value_size(uint16_t format)
{
    uint32_t n = 0;
    for (uint16_t b = format; b; b >>= 1) n += (b & 1);
    return n * 2;
}

// The x-advance adjustment for the first glyph of a pair, in font units, or 0.
static int ntf_sc_kern(const NtfScFont *f, uint16_t a, uint16_t b)
{
    const NtfTable t = { (const uint8_t *)f->gpos.constData(), (uint32_t)f->gpos.size() };
    for (int i = 0; i < f->kern_subtables.size(); ++i) {
        const uint32_t sub = f->kern_subtables[i];
        const uint16_t format = t.u16(sub);
        const int ci = ntf_coverage_index(t, sub + t.u16(sub + 2), a);
        if (ci < 0) continue;
        const uint16_t vf1 = t.u16(sub + 4), vf2 = t.u16(sub + 6);
        const uint32_t s1 = ntf_value_size(vf1), s2 = ntf_value_size(vf2);
        // XAdvance is the third field; XPlacement and YPlacement precede it when present.
        if (!(vf1 & 0x0004)) continue;
        const uint32_t xadv_at = ((vf1 & 1) ? 2 : 0) + ((vf1 & 2) ? 2 : 0);
        if (format == 1) {
            const uint16_t set_count = t.u16(sub + 8);
            if (ci >= (int)set_count || !t.ok(sub + 10, (uint32_t)set_count * 2)) continue;
            const uint32_t set = sub + t.u16(sub + 10 + (uint32_t)ci * 2);
            const uint16_t pairs = t.u16(set);
            const uint32_t rec = 2 + s1 + s2;
            if (!t.ok(set + 2, (uint32_t)pairs * rec)) continue;
            int lo = 0, hi = (int)pairs - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                const uint32_t r = set + 2 + (uint32_t)mid * rec;
                const uint16_t second = t.u16(r);
                if (second == b) return t.s16(r + 2 + xadv_at);
                if (second < b) lo = mid + 1; else hi = mid - 1;
            }
            continue;                       // covered but not paired: the next subtable may apply
        }
        if (format == 2) {
            const uint16_t c1 = ntf_class_of(t, sub + t.u16(sub + 8), a);
            const uint16_t c2 = ntf_class_of(t, sub + t.u16(sub + 10), b);
            const uint16_t n1 = t.u16(sub + 12), n2 = t.u16(sub + 14);
            if (c1 >= n1 || c2 >= n2) continue;
            const uint32_t rec = s1 + s2;
            const uint32_t r = sub + 16 + ((uint32_t)c1 * n2 + c2) * rec;
            if (!t.ok(r, rec)) continue;
            // A class table applies to every covered first glyph, even at a zero value. This is
            // where the pair is decided, as it is in HarfBuzz.
            return t.s16(r + xadv_at);
        }
    }
    return 0;
}

static QFontEngine *ntf_sc_primary(QFontEngine *fe)
{
    if (fe && fe->type() == QFontEngine::Multi)
        return static_cast<QFontEngineMulti *>(fe)->engine(0);
    return fe;
}

// The record for a face, built on first sight. Records are never freed: another thread may be
// reading one, and a session sees a handful of faces at most. Past the cap a face is treated as
// having no small caps, which is the stock outcome.
static NtfScFont *ntf_sc_font_for(QFontEngine *fe)
{
    fe = ntf_sc_primary(fe);
    if (!fe) return 0;
    const unsigned long id = ntf_sc_face_identity(fe);

    pthread_mutex_lock(&ntf_sc_lock);
    for (int i = 0; i < ntf_sc_font_count; ++i) {
        if (ntf_sc_fonts[i]->id == id) {
            NtfScFont *f = ntf_sc_fonts[i];
            pthread_mutex_unlock(&ntf_sc_lock);
            return f;
        }
    }
    if (ntf_sc_font_count >= NTF_SC_MAX_FONTS) {
        pthread_mutex_unlock(&ntf_sc_lock);
        return 0;
    }

    NtfScFont *f = new NtfScFont();
    f->id = id;
    f->has_smcp = false;
    f->upem = 0;
    f->num_glyphs = 0;
    f->smcp = 0;

    const QByteArray head = fe->getSfntTable(NTF_TAG('h', 'e', 'a', 'd'));
    const QByteArray maxp = fe->getSfntTable(NTF_TAG('m', 'a', 'x', 'p'));
    if (head.size() >= 20 && maxp.size() >= 6) {
        const NtfTable th = { (const uint8_t *)head.constData(), (uint32_t)head.size() };
        const NtfTable tm = { (const uint8_t *)maxp.constData(), (uint32_t)maxp.size() };
        f->upem = th.u16(18);
        f->num_glyphs = tm.u16(4);
    }
    if (f->upem && f->num_glyphs) {
        f->gsub = fe->getSfntTable(NTF_TAG('G', 'S', 'U', 'B'));
        if (f->gsub.size() >= 10) {
            ntf_sc_read_smcp(f);
            if (f->has_smcp) {
                ntf_sc_read_ligatures(f);
                f->gpos = fe->getSfntTable(NTF_TAG('G', 'P', 'O', 'S'));
                if (f->gpos.size() >= 10) ntf_sc_read_kerning(f);
            }
        }
        f->gsub = QByteArray();           // the map is built; the table is not needed again
    }
    if (!f->has_smcp) {
        free(f->smcp);
        f->smcp = 0;
        f->gpos = QByteArray();
    }

    ntf_sc_fonts[ntf_sc_font_count++] = f;
    pthread_mutex_unlock(&ntf_sc_lock);

    const QByteArray fam = fe->fontDef.family.toUtf8();
    if (f->has_smcp)
        ntf_sc_log("small caps: %s (%s) has smcp, %d ligature forms, %d kern subtables",
                   fam.constData(), fe->fontDef.styleName.toUtf8().constData(),
                   f->ligatures.size(), f->kern_subtables.size());
    else
        ntf_sc_log("small caps: %s (%s) has no smcp; stock shrunken capitals", fam.constData(),
                   fe->fontDef.styleName.toUtf8().constData());
    return f;
}

// ---- seam 1: QTextEngine::fontEngine -------------------------------------------------------------

typedef QFontEngine *(*ntf_font_engine_fn)(const QTextEngine *, const QScriptItem &, QFixed *,
                                           QFixed *, QFixed *);
static ntf_font_engine_fn ntf_original_font_engine = 0;

static QFontEngine *ntf_font_engine_entry(const QTextEngine *e, const QScriptItem &si,
                                          QFixed *ascent, QFixed *descent, QFixed *leading)
{
    if (!ntf_sc_active || si.analysis.flags != QScriptAnalysis::SmallCaps)
        return ntf_original_font_engine(e, si, ascent, descent, leading);

    // The same item with the flag cleared resolves to the full-size engine. Ascent and descent
    // come from that engine in both branches of the original, so they are right either way.
    QScriptItem plain = si;
    plain.analysis.flags = QScriptAnalysis::None;
    QFontEngine *full = ntf_original_font_engine(e, plain, ascent, descent, leading);
    if (full) {
        const NtfScFont *f = ntf_sc_font_for(full);
        if (f && f->has_smcp) return full;
    }
    return ntf_original_font_engine(e, si, ascent, descent, leading);
}

// ---- seam 2: the shaping post-pass -------------------------------------------------------------

static const NtfScLigature *ntf_sc_find_ligature(const NtfScFont *f, uint16_t glyph)
{
    for (int i = 0; i < f->ligatures.size(); ++i)
        if (f->ligatures[i].ligature == glyph) return &f->ligatures[i];
    return 0;
}

// Open ligatures back up into their components, so each letter can take its small cap. The
// shaper's glyph count grows by the components it adds, and there is always room for it: the
// engine reserved one glyph slot per character before shaping, and a ligature never covers more
// characters than it has components.
static int ntf_sc_expand_ligatures(const NtfScFont *f, QGlyphLayout &g, unsigned short *lc,
                                   int itemLength, int n)
{
    if (f->ligatures.isEmpty()) return n;
    for (int i = 0; i < n; ++i) {
        const glyph_t gl = g.glyphs[i];
        if (gl >> 24) continue;
        const NtfScLigature *L = ntf_sc_find_ligature(f, (uint16_t)(gl & 0xffff));
        if (!L) continue;

        int first = -1, chars = 0;
        for (int c = 0; c < itemLength; ++c) {
            if (lc[c] != i) continue;
            if (first < 0) first = c;
            chars++;
        }
        if (first < 0 || chars != L->count) continue;
        bool contiguous = true;
        for (int c = first; c < first + chars; ++c) if (lc[c] != i) contiguous = false;
        if (!contiguous) continue;

        const int extra = L->count - 1;
        if (n + extra > g.numGlyphs) return n;

        const int tail = n - (i + 1);
        if (tail > 0) {
            memmove(g.glyphs + i + 1 + extra, g.glyphs + i + 1, tail * sizeof(glyph_t));
            memmove(g.advances_x + i + 1 + extra, g.advances_x + i + 1, tail * sizeof(QFixed));
            memmove(g.advances_y + i + 1 + extra, g.advances_y + i + 1, tail * sizeof(QFixed));
            memmove(g.offsets + i + 1 + extra, g.offsets + i + 1, tail * sizeof(QFixedPoint));
            memmove(g.attributes + i + 1 + extra, g.attributes + i + 1, tail * sizeof(QGlyphAttributes));
            memmove(g.justifications + i + 1 + extra, g.justifications + i + 1, tail * sizeof(QGlyphJustification));
        }
        const QGlyphAttributes attrs = g.attributes[i];
        for (int j = 0; j < L->count; ++j) {
            g.glyphs[i + j] = L->components[j];
            g.advances_x[i + j] = 0;
            g.advances_y[i + j] = 0;
            g.offsets[i + j] = QFixedPoint();
            g.attributes[i + j] = attrs;
            g.attributes[i + j].clusterStart = 1;
            memset(&g.justifications[i + j], 0, sizeof(QGlyphJustification));
        }
        for (int c = 0; c < itemLength; ++c) if (lc[c] > i) lc[c] += extra;
        for (int j = 0; j < chars; ++j) lc[first + j] = (unsigned short)(i + j);
        n += extra;
        i += extra;
    }
    return n;
}

int ntf_smallcaps_shape(const QTextEngine *e, const QScriptItem &si, const unsigned short *string,
                        int itemLength, QFontEngine *fontEngine,
                        const QVector<unsigned int> &itemBoundaries, bool kerningEnabled,
                        ntf_shape_fn original)
{
    (void)string;
    if (!ntf_sc_active || si.analysis.flags != QScriptAnalysis::SmallCaps) return -1;
    if (!e->layoutData || itemLength <= 0) return -1;
    if (si.analysis.bidiLevel & 1) return -1;              // a right-to-left run: leave it alone
    const NtfScFont *f = ntf_sc_font_for(fontEngine);
    if (!f || !f->has_smcp || !f->smcp) return -1;

    // The text the item really holds, before shapeText uppercased its copy.
    const QString &text = e->layoutData->string;
    if (si.position < 0 || si.position + itemLength > text.length()) return -1;
    const unsigned short *lower = reinterpret_cast<const unsigned short *>(text.constData()) + si.position;

    const int n0 = original(e, si, lower, itemLength, fontEngine, itemBoundaries, kerningEnabled);
    if (n0 <= 0) return n0;

    QGlyphLayout g = e->availableGlyphs(&si);
    unsigned short *lc = e->logClusters(&si);
    if (!g.glyphs || !lc || n0 > g.numGlyphs) return n0;

    int n = ntf_sc_expand_ligatures(f, g, lc, itemLength, n0);

    int substituted = 0;
    for (int i = 0; i < n; ++i) {
        const glyph_t gl = g.glyphs[i];
        if (gl >> 24) continue;                              // a fallback face: not this font
        const uint16_t id = (uint16_t)(gl & 0xffff);
        if (id < f->num_glyphs && f->smcp[id]) { g.glyphs[i] = f->smcp[id]; substituted++; }
    }
    if (!substituted && n == n0) return n;

    // Advances from the engine for the new glyphs, then the font's own pair kerning between them,
    // scaled to the engine's pixel size. This replaces the kerning the shaper computed for the
    // lowercase glyphs, which no longer applies.
    QFontEngine *primary = ntf_sc_primary(fontEngine);
    QGlyphLayout sub = g.mid(0, n);
    const QFontEngine::ShaperFlags flags = e->option.useDesignMetrics()
        ? QFontEngine::ShaperFlags(QFontEngine::DesignMetrics) : QFontEngine::ShaperFlags(0);
    fontEngine->recalcAdvances(&sub, flags);

    if (kerningEnabled && primary && !f->kern_subtables.isEmpty() && f->upem) {
        const qreal scale = primary->fontDef.pixelSize / (qreal)f->upem;
        const bool integer = (primary->fontDef.styleStrategy & QFont::ForceIntegerMetrics) != 0;
        for (int i = 0; i + 1 < n; ++i) {
            const glyph_t a = g.glyphs[i], b = g.glyphs[i + 1];
            if ((a >> 24) || (b >> 24)) continue;
            const int k = ntf_sc_kern(f, (uint16_t)(a & 0xffff), (uint16_t)(b & 0xffff));
            if (!k) continue;
            const qreal px = k * scale;
            g.advances_x[i] += integer ? QFixed(qRound(px)) : QFixed::fromReal(px);
        }
    }
    return n;
}

// ---- installation ------------------------------------------------------------------------------

extern "C" ntf_smallcaps_status_t ntf_smallcaps_enable(void *font_engine_sym,
                                                       bool shaper_detour_running,
                                                       ntf_smallcaps_logger logger)
{
    ntf_smallcaps_status_t status;
    status.installed = false;
    ntf_sc_logger = logger;
    if (!font_engine_sym || !shaper_detour_running) return status;

    // The fontEngine detour on its own would shape the uppercased text at full size, which is
    // full capitals, so it is only installed once the shaper detour is in place to do its half.
    const int rc = ntf_detour_at(font_engine_sym, (void *)&ntf_font_engine_entry,
                                 (void **)&ntf_original_font_engine, 0);
    if (rc != 0) {
        ntf_sc_log("small caps: could not detour QTextEngine::fontEngine (%d); sitting out", rc);
        return status;
    }
    ntf_sc_active = true;
    status.installed = true;
    return status;
}
