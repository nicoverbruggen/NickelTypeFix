// FIX 12 — fast text shaping (libQtGui / QTextEngine)
//
// Opening a long kepub chapter stalls for seconds, and 85% of that stall is text shaping. Two
// separate causes, fixed here together because each one exposes the other.
//
// First, Kobo's reader shapes through Qt 5.2's OLD shaper, a 2007 implementation kept for
// compatibility, not the HarfBuzz NG that ships in the same library. It applies a font's GPOS
// lookups wholesale, so its cost scales with the font's tables; measured on a real chapter it is
// 2.5x slower than NG for the same text. Qt picks between the two with one internal flag.
//
// Second, nothing anywhere caches a shaping result. HarfBuzz caches its shape plan, not its
// output, and it cannot cache the output because the caller owns the buffer and may vary features
// per call. WebKit's word cache only covers its simple path, and any font with GPOS takes the
// complex path. So every occurrence of a word is shaped from scratch. Measured on a real chapter:
// 28,541 shaping calls, of which 24,373 asked for text already shaped with the same font and
// settings.
//
// Together these take that chapter's relayout from 2029 ms to 476 ms, measured through the
// device's own Qt, QtWebKit and font engine. Neither changes a glyph: the cache records what the
// real shaper produced and replays exactly that, and both were verified to render pixel-identical
// to stock.
//
// Everything here fails closed. If the flag cannot be located, or either shaper symbol is
// missing, the fix reports that it sat out and the reader runs stock.
//
// Reaching QTextEngine's internals needs Qt's private headers, which is why this lives in its own
// file: the `private`/`protected` redefinition below must not leak into the rest of the mod. It is
// a hack against headers we compile ourselves, not against the device, and it means the compiler
// computes every structure offset instead of us reading them out of a disassembly.

#define private public
#define protected public
#include <QtGui/private/qtextengine_p.h>
#undef private
#undef protected
#include <QtGui/private/qfontengine_p.h>
#include <QtCore/QVector>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shape_cache.h"

// Qt 5.2's newer shaper does not fill in the justification class. shapeTextWithHarfbuzzNG writes
// only clusterStart, while the older path hands the whole attributes array to HarfBuzz, which sets
// justification per glyph. QTextEngine::justify builds its distribution points from exactly that
// field, so with the newer shaper it finds none inside a text run: the line still reaches the
// margin, because WebKit stretches the gaps between runs, but every space within a run stays
// unstretched. In a kepub each sentence is its own koboSpan, so all the stretch lands on the
// spaces after full stops. Measured on a real chapter through the device's own engine, that takes
// the widest space on a line from 14px to 56px while the median stays at 6px.
//
// Marking a space as a justification point is what the older shaper did, so this restores the
// input QTextEngine::justify expects rather than changing what it does. Only glyphs the shaper
// left unclassified are touched, so the older path -- and Arabic runs, which use their own
// classes -- are never disturbed.
static void ntf_fill_justification(const QTextEngine *e, const QScriptItem &si,
                                   const ushort *string, int itemLength, int num_glyphs)
{
    if (!e->layoutData) return;
    QGlyphLayout g = e->availableGlyphs(&si).mid(0, num_glyphs);
    const unsigned short *log_clusters = e->logClusters(&si);
    if (!log_clusters || !g.attributes) return;
    for (int c = 0; c < itemLength; c++) {
        if (string[c] != 0x0020) continue;                 // an ordinary space, nothing else
        const unsigned short gi = log_clusters[c];
        if (gi >= (unsigned)num_glyphs) continue;
        if (g.attributes[gi].justification != QGlyphAttributes::NoJustification) continue;
        g.attributes[gi].justification = QGlyphAttributes::Space;
    }
}

static int ntf_install_cache(void *sym);
extern "C" int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out);

typedef int (*ShapeFn)(const QTextEngine *, const QScriptItem &, const ushort *, int,
                       QFontEngine *, const QVector<uint> &, bool);
static ShapeFn ntf_original_shape = 0;

// The classification has to happen wherever the real shaper runs. Two paths reach it without
// going through the cache -- items longer than the cache will copy, and items that find no room to
// replay into -- and in a kepub a koboSpan is a whole sentence, so the long-item path is common.
// Leaving those unclassified puts the stretch back on the sentence gaps for exactly those lines.
static int ntf_shape_and_classify(const QTextEngine *e, const QScriptItem &si,
                                  const ushort *string, int itemLength, QFontEngine *fontEngine,
                                  const QVector<uint> &itemBoundaries, bool kerningEnabled)
{
    const int n = ntf_original_shape(e, si, string, itemLength, fontEngine, itemBoundaries,
                                     kerningEnabled);
    if (n > 0) ntf_fill_justification(e, si, string, itemLength, n);
    return n;
}


namespace {
unsigned long ntf_cache_stored = 0;   // records held
}   // namespace

// Two things make this unsafe if left alone.
//
// The table is shared, and Nickel shapes on more than the reader thread. A lock costs a fraction
// of a microsecond against the tens of microseconds a shaping call takes.
//
// The other is the cache key. Keying on the QFontEngine pointer looks natural and is wrong: Qt's
// font cache destroys engines on a timer, so a later allocation can land on the same address with
// a different face. An earlier version defended against that by holding a reference on every
// engine it cached against, capped at 64 — which turned out to be a cliff. Once a device had seen
// 64 engines, which browsing a library does easily, nothing new could be cached and the whole fix
// silently stopped working. Measured: 53 hits instead of 26,549.
//
// Key on what the engine IS instead of where it lives. Two engines resolved from the same QFontDef
// shape identically, so the description is the correct identity. No references, no cap, no leak,
// and a recycled address simply computes a different key.
static pthread_mutex_t ntf_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long ntf_engine_identity(const QFontEngine *fe)
{
    if (!fe) return 0;
    unsigned long h = 2166136261UL;
    #define NTF_MIX_BYTES(ptr, len) do { \
        const unsigned char *b = (const unsigned char *)(ptr); \
        for (size_t i = 0; i < (size_t)(len); ++i) { h ^= b[i]; h *= 16777619UL; } \
    } while (0)
    #define NTF_MIX(v) do { typeof(v) tmp_ = (v); NTF_MIX_BYTES(&tmp_, sizeof tmp_); } while (0)

    const QFontDef &d = fe->fontDef;
    const QByteArray fam = d.family.toUtf8();
    NTF_MIX_BYTES(fam.constData(), fam.size());
    const QByteArray sty = d.styleName.toUtf8();
    NTF_MIX_BYTES(sty.constData(), sty.size());
    NTF_MIX(d.pixelSize);
    NTF_MIX(d.pointSize);
    NTF_MIX(d.weight);
    NTF_MIX(d.style);
    NTF_MIX(d.stretch);
    NTF_MIX(d.styleHint);
    NTF_MIX(d.styleStrategy);
    NTF_MIX(d.hintingPreference);
    NTF_MIX(d.fixedPitch);
    const int t = (int)fe->type();
    NTF_MIX(t);
    #undef NTF_MIX
    #undef NTF_MIX_BYTES
    return h ? h : 1;
}

struct NtfRecord {
    unsigned long hash;
    unsigned long engine_id;   // QFontDef-derived identity, not an address
    ushort *text;
    uint text_length;
    unsigned key_bits;             // kerning, bidi parity, script, sub-engine index
    uint num_glyphs;
    glyph_t *glyphs;
    QFixed *advances_x;
    QFixed *advances_y;
    QFixedPoint *offsets;
    QGlyphAttributes *attributes;
    ushort *log_clusters;          // text_length entries, as the original wrote them
};

static const int NTF_CACHE_SLOTS = 32768;   // open addressed, fixed size, never rehashed
static const int NTF_CACHE_LIMIT = 12000;   // stop recording well before the table crowds
static const uint NTF_CACHE_MAX_TEXT = 96;  // longer items are rare and not worth copying
static NtfRecord *ntf_cache[NTF_CACHE_SLOTS];

static unsigned long ntf_hash(unsigned long engine_id, const ushort *text, uint len,
                              unsigned key_bits, const QVector<uint> &boundaries)
{
    unsigned long h = 2166136261UL;
    const unsigned char *p = (const unsigned char *)&engine_id;
    for (unsigned i = 0; i < sizeof engine_id; ++i) { h ^= p[i]; h *= 16777619UL; }
    h ^= key_bits; h *= 16777619UL;
    for (uint i = 0; i < len; ++i) {
        h ^= text[i] & 0xff; h *= 16777619UL;
        h ^= text[i] >> 8;   h *= 16777619UL;
    }
    // Item boundaries change how the text is split across sub-fonts, so they are part of the key.
    for (int i = 0; i < boundaries.size(); ++i) { h ^= boundaries[i]; h *= 16777619UL; }
    h ^= (unsigned long)boundaries.size(); h *= 16777619UL;
    return h ? h : 1;
}

static NtfRecord **ntf_slot(unsigned long h, unsigned long engine_id, const ushort *text,
                            uint len, unsigned key_bits)
{
    unsigned idx = (unsigned)(h % NTF_CACHE_SLOTS);
    for (unsigned probe = 0; probe < 48; ++probe) {
        NtfRecord **slot = &ntf_cache[(idx + probe) % NTF_CACHE_SLOTS];
        NtfRecord *r = *slot;
        if (!r) return slot;
        if (r->hash == h && r->engine_id == engine_id && r->text_length == len && r->key_bits == key_bits
            && memcmp(r->text, text, len * sizeof(ushort)) == 0)
            return slot;
    }
    return 0;
}

static void ntf_record(NtfRecord **slot, unsigned long h, unsigned long engine_id, const ushort *text,
                       uint len, unsigned key_bits, uint num_glyphs,
                       const QGlyphLayout &g, const ushort *log_clusters)
{
    if (ntf_cache_stored >= NTF_CACHE_LIMIT) return;
    NtfRecord *r = (NtfRecord *)calloc(1, sizeof(NtfRecord));
    if (!r) return;
    r->text         = (ushort *)malloc(len * sizeof(ushort));
    r->glyphs       = (glyph_t *)malloc(num_glyphs * sizeof(glyph_t));
    r->advances_x   = (QFixed *)malloc(num_glyphs * sizeof(QFixed));
    r->advances_y   = (QFixed *)malloc(num_glyphs * sizeof(QFixed));
    r->offsets      = (QFixedPoint *)malloc(num_glyphs * sizeof(QFixedPoint));
    r->attributes   = (QGlyphAttributes *)malloc(num_glyphs * sizeof(QGlyphAttributes));
    r->log_clusters = (ushort *)malloc(len * sizeof(ushort));
    if (!r->text || !r->glyphs || !r->advances_x || !r->advances_y || !r->offsets
        || !r->attributes || !r->log_clusters) {
        free(r->text); free(r->glyphs); free(r->advances_x); free(r->advances_y);
        free(r->offsets); free(r->attributes); free(r->log_clusters); free(r);
        return;
    }
    memcpy(r->text,       text,         len * sizeof(ushort));
    memcpy(r->glyphs,     g.glyphs,     num_glyphs * sizeof(glyph_t));
    memcpy(r->advances_x, g.advances_x, num_glyphs * sizeof(QFixed));
    memcpy(r->advances_y, g.advances_y, num_glyphs * sizeof(QFixed));
    memcpy(r->offsets,    g.offsets,    num_glyphs * sizeof(QFixedPoint));
    memcpy(r->attributes, g.attributes, num_glyphs * sizeof(QGlyphAttributes));
    memcpy(r->log_clusters, log_clusters, len * sizeof(ushort));
    r->hash = h; r->engine_id = engine_id; r->text_length = len; r->key_bits = key_bits;
    r->num_glyphs = num_glyphs;
    *slot = r;
    ntf_cache_stored++;
}

extern "C" int ntf_cache_entry(const QTextEngine *e, const QScriptItem &si, const ushort *string,
                               int itemLength, QFontEngine *fontEngine,
                               const QVector<uint> &itemBoundaries, bool kerningEnabled)
{

    if (itemLength <= 0 || (uint)itemLength > NTF_CACHE_MAX_TEXT || !e->layoutData) {
        return ntf_shape_and_classify(e, si, string, itemLength, fontEngine, itemBoundaries,
                                      kerningEnabled);
    }

    // Everything the original reads that is not the text itself. The sub-font index comes from the
    // glyph indices stringToCMap already wrote, so identical text can still land on a different
    // face; keeping it in the key stops one item's result standing in for another's.
    const QGlyphLayout available = e->availableGlyphs(&si);
    const unsigned sub_engine = available.numGlyphs > 0 ? (available.glyphs[0] >> 24) : 0;
    const unsigned key_bits = (kerningEnabled ? 1u : 0u)
                            | ((si.analysis.bidiLevel & 1u) << 1)
                            | ((unsigned)si.analysis.script << 2)
                            | (sub_engine << 12);

    const unsigned long engine_id = ntf_engine_identity(fontEngine);
    const unsigned long h = ntf_hash(engine_id, string, (uint)itemLength, key_bits, itemBoundaries);

    pthread_mutex_lock(&ntf_cache_lock);
    NtfRecord **slot = ntf_slot(h, engine_id, string, (uint)itemLength, key_bits);

    if (slot && *slot) {
        const NtfRecord *r = *slot;
        // Replaying skips the original's reallocation, so there has to be room already.
        if (e->layoutData->glyphLayout.numGlyphs - e->layoutData->used >= (int)r->num_glyphs) {
            QGlyphLayout g = available.mid(0, r->num_glyphs);
            memcpy(g.glyphs,     r->glyphs,     r->num_glyphs * sizeof(glyph_t));
            memcpy(g.advances_x, r->advances_x, r->num_glyphs * sizeof(QFixed));
            memcpy(g.advances_y, r->advances_y, r->num_glyphs * sizeof(QFixed));
            memcpy(g.offsets,    r->offsets,    r->num_glyphs * sizeof(QFixedPoint));
            memcpy(g.attributes, r->attributes, r->num_glyphs * sizeof(QGlyphAttributes));
            memcpy(e->logClusters(&si), r->log_clusters, r->text_length * sizeof(ushort));
            pthread_mutex_unlock(&ntf_cache_lock);
            return (int)r->num_glyphs;
        }
        pthread_mutex_unlock(&ntf_cache_lock);
        return ntf_shape_and_classify(e, si, string, itemLength, fontEngine, itemBoundaries,
                                      kerningEnabled);
    }

    // The original runs with the lock held. It is reentrant only through us, and it does not
    // shape recursively, so this cannot deadlock; holding it keeps the slot we found valid.
    // Classifies before recording, so a replayed hit carries what a fresh shape would.
    const int n = ntf_shape_and_classify(e, si, string, itemLength, fontEngine, itemBoundaries,
                                         kerningEnabled);
    if (slot && n > 0)
        ntf_record(slot, h, engine_id, string, (uint)itemLength, key_bits, (uint)n,
                   e->availableGlyphs(&si).mid(0, n), e->logClusters(&si));
    pthread_mutex_unlock(&ntf_cache_lock);
    return n;
}

// ---- installation ------------------------------------------------------------------------------
// Detour the shaper's prologue. Its first three instructions are position independent, so they can
// be relocated into a trampoline and the original stays reachable through it.

static const int NTF_DETOUR_BYTES = 8;

static bool ntf_write_code(void *dst, const void *src, unsigned n)
{
    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned long lo = (unsigned long)dst & ~(unsigned long)(pagesize - 1);
    unsigned long hi = ((unsigned long)dst + n + pagesize - 1) & ~(unsigned long)(pagesize - 1);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(dst, src, n);
    __builtin___clear_cache((char *)dst, (char *)dst + n);
    return true;
}

// ldr.w pc, [pc, #0] ; .word target|1
static void ntf_absolute_jump(unsigned char *out, void *target)
{
    out[0] = 0xdf; out[1] = 0xf8; out[2] = 0x00; out[3] = 0xf0;
    unsigned long t = (unsigned long)target | 1UL;
    memcpy(out + 4, &t, 4);
}

// Switch Qt to HarfBuzz NG by finding its useHarfbuzzNG flag and setting it.
//
// Redirecting the old shaper to the new one looks tempting, since both are exported with the same
// signature and shapeText is the only caller of either. It crashes: the flag is read in twenty
// places in this library, not just the dispatch, so leaving it clear while the shaper changes puts
// Qt in a mixed state. The flag itself has to move.
//
// It is a stripped local static with no symbol, reached through the GOT, so there is nothing to
// dlsym and a fixed offset would only be right on the firmware it was measured on. shapeText is
// exported, though, and it reads the flag in the clear, so its own instructions say where the flag
// is. Decoding them gives an address that is correct on any build with this code shape, and every
// step is checked so an unrecognised build sits the fix out instead of writing somewhere wrong.
//
//   ldr  r3, [pc, #N]     <- N indexes a literal holding the flag's GOT offset
//   ldr  r2, [r7, #M]     <- the GOT base, saved in the prologue
//   ldr  r3, [r2, r3]     <- &useHarfbuzzNG
//   ldrb r3, [r3, #0]
//   cmp  r3, #0
//
// and in the prologue, for the GOT base:
//
//   ldr  r3, [pc, #N2]
//   add  r3, pc

static inline unsigned short ntf_hw(const unsigned char *p) { return p[0] | (p[1] << 8); }

// A Thumb `ldr rX, [pc, #imm8*4]` reads from Align(instruction + 4, 4) + imm8 * 4.
static const unsigned char *ntf_literal_for(const unsigned char *at)
{
    unsigned long base = ((unsigned long)at + 4) & ~3UL;
    return (const unsigned char *)(base + (ntf_hw(at) & 0xff) * 4UL);
}

static bool ntf_is_ldr_r3_pc(const unsigned char *at) { return (ntf_hw(at) & 0xff00) == 0x4b00; }

static unsigned char *ntf_find_ng_flag(void *sym)
{
    if (!sym) return 0;
    const unsigned char *fn = (const unsigned char *)((unsigned long)sym & ~1UL);

    Dl_info info;
    if (!dladdr(sym, &info) || !info.dli_fbase) return 0;

    // The GOT base: the first `add r3, pc` in the prologue, with the `ldr r3, [pc, #N]` before it.
    unsigned long got = 0;
    for (int i = 0; i < 0x40 && !got; i += 2) {
        if (ntf_hw(fn + i) != 0x447b) continue;              // add r3, pc
        for (int j = i - 2; j >= 0 && j > i - 12; j -= 2) {
            if (!ntf_is_ldr_r3_pc(fn + j)) continue;
            unsigned long v = *(const unsigned long *)ntf_literal_for(fn + j);
            got = (unsigned long)(fn + i) + 4 + v;
            break;
        }
    }
    if (!got) return 0;

    // The read itself: ldr r3,[r2,r3] ; ldrb r3,[r3,#0], with cmp r3,#0 close behind. Requiring
    // the compare keeps this from matching an unrelated indexed load of a byte.
    for (int i = 0; i < 0x600; i += 2) {
        if (ntf_hw(fn + i) != 0x58d3 || ntf_hw(fn + i + 2) != 0x781b) continue;
        bool compared = false;
        for (int k = 4; k <= 10 && !compared; k += 2)
            if (ntf_hw(fn + i + k) == 0x2b00) compared = true;
        if (!compared) continue;

        // Walk back to the `ldr r3, [pc, #N]` that loaded the GOT offset.
        for (int j = i - 2; j >= 0 && j > i - 16; j -= 2) {
            if (!ntf_is_ldr_r3_pc(fn + j)) continue;
            unsigned long off = *(const unsigned long *)ntf_literal_for(fn + j);
            unsigned char **slot = (unsigned char **)(got + off);
            unsigned char *flag = *slot;
            // Fail closed: the flag must sit in this library and hold a boolean.
            if (!flag || flag < (unsigned char *)info.dli_fbase
                || flag > (unsigned char *)info.dli_fbase + 0x2000000UL
                || (*flag != 0 && *flag != 1))
                return 0;
            return flag;
        }
    }
    return 0;
}

static int ntf_enable_harfbuzz_ng(void *shape_text)
{
    unsigned char *flag = ntf_find_ng_flag(shape_text);
    if (!flag) return -1;
    if (*flag) return 1;            // already on; nothing to do
    *flag = 1;
    return 0;
}


// Detour the shaper's prologue so every call reaches the cache first. Its first three
// instructions are position independent, so they relocate into a trampoline and the original stays
// reachable through it, which is what lets a miss fall through to the real shaper.
// which: 0 for the old shaper, 1 for HarfBuzz NG. Cache whichever one the firmware will run.
static void ntf_disable_harfbuzz_ng(void *shape_text)
{
    unsigned char *flag = ntf_find_ng_flag(shape_text);
    if (flag) *flag = 0;
}

static int ntf_install_cache(void *sym)
{
    // Through the checked detour, not a second copy of it. An eight-byte copy that does not decode
    // instruction boundaries runs half an instruction on any firmware whose prologue differs, and
    // this installs at init while the boot failsafe is still armed.
    if (!sym) return -1;
    return ntf_detour_at(sym, (void *)&ntf_cache_entry, (void **)&ntf_original_shape, 0);
}


// A general prologue detour, exposed so probes can observe functions that libnickel does not call
// through the PLT. WebKit emits its own lifecycle signals from inside QtWebKitWidgets, so a PLT
// hook cannot see them; detouring the emitter can. Same mechanism the shaper cache uses: relocate
// the first 8 bytes into a trampoline, write an absolute jump over them, and hand the trampoline
// back so the original stays callable.

// Detour an arbitrary address, relocating whole instructions.
//
// The 8-byte absolute jump has to land on an instruction boundary or the trampoline executes half
// of one. Thumb-2 mixes 2- and 4-byte instructions, so decode forward until at least 8 bytes are
// covered. Refuse anything PC-relative: those read their operand from where they sit, so moving
// them into a trampoline silently changes what they load.
static bool ntf_thumb_is_32bit(unsigned short hw)
{
    unsigned short top = hw & 0xf800;
    return top == 0xe800 || top == 0xf000 || top == 0xf800;
}

// Conservative: any encoding that can reference PC, plus every branch.
static bool ntf_thumb_uses_pc(const unsigned char *p, int len)
{
    unsigned short hw = (unsigned short)(p[0] | (p[1] << 8));
    if (len == 2) {
        if ((hw & 0xf800) == 0x4800) return true;            // ldr rX, [pc, #imm]
        if ((hw & 0xf800) == 0xa000) return true;            // adr rX, label
        if ((hw & 0xff78) == 0x4468) return true;            // add rX, pc
        if ((hw & 0xf000) == 0xd000) return true;            // b<cond>
        if ((hw & 0xf800) == 0xe000) return true;            // b
        if ((hw & 0xff87) == 0x4700) return true;            // bx/blx reg
        if ((hw & 0xf500) == 0xb100) return true;            // cbz/cbnz
        return false;
    }
    unsigned short hw2 = (unsigned short)(p[2] | (p[3] << 8));
    if (hw == 0xf8df || hw == 0xf85f) return true;           // ldr.w rX, [pc, #imm]
    if ((hw & 0xfbff) == 0xf2af) return true;                // adr.w
    if ((hw & 0xf800) == 0xf000 && (hw2 & 0x8000)) return true;  // b.w / bl / blx
    return false;
}

extern "C" int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out)
{
    if (!addr || !replacement || !original) return -1;
    unsigned char *fn = (unsigned char *)((unsigned long)addr & ~1UL);
    if ((unsigned long)fn & 3UL) return -5;      // the literal load needs a 4-byte aligned target

    int n = 0;
    while (n < NTF_DETOUR_BYTES) {
        unsigned short hw = (unsigned short)(fn[n] | (fn[n + 1] << 8));
        int len = ntf_thumb_is_32bit(hw) ? 4 : 2;
        if (ntf_thumb_uses_pc(fn + n, len)) return -6;
        n += len;
        if (n > 32) return -7;
    }
    if (relocated_out) *relocated_out = n;

    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned char *tramp = (unsigned char *)mmap(0, pagesize, PROT_READ | PROT_WRITE | PROT_EXEC,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return -3;

    // `ldr.w pc, [pc, #0]` reads its literal from Align(PC, 4), so the jump instruction must sit
    // on a 4-byte boundary or the CPU rounds down and loads the wrong word. At the start of a
    // function that is free; after relocating an odd number of halfwords it is not. Pad with a
    // nop so the jump lands aligned, and keep jumping back to the true end of what was moved.
    memcpy(tramp, fn, n);
    int jump_at = n;
    if (jump_at & 3) {
        tramp[jump_at] = 0x00; tramp[jump_at + 1] = 0xbf;   // nop
        jump_at += 2;
    }
    ntf_absolute_jump(tramp + jump_at, fn + n);
    __builtin___clear_cache((char *)tramp, (char *)tramp + jump_at + 8);
    *original = (void *)((unsigned long)tramp | 1UL);

    unsigned char detour[NTF_DETOUR_BYTES];
    ntf_absolute_jump(detour, replacement);
    if (!ntf_write_code(fn, detour, NTF_DETOUR_BYTES)) {
        // Nothing reaches the trampoline if the jump was never written, and leaving it mapped
        // would also leave *original pointing at code the caller must not run.
        munmap(tramp, pagesize);
        *original = 0;
        return -4;
    }
    return 0;
}


extern "C" ntf_shape_status_t ntf_shape_cache_enable(void *shape_text, void *shaper_old,
                                                     void *shaper_ng)
{
    ntf_shape_status_t status;
    status.ng_enabled = false;
    status.cache_installed = false;

    // Order matters. Switch the engine first, then cache in front of whichever one will run, so a
    // failure to switch still leaves the cache on the shaper that is actually going to be used.
    bool switched_here = false;
    if (shaper_ng) {
        const int ng = ntf_enable_harfbuzz_ng(shape_text);
        status.ng_enabled = (ng == 0 || ng == 1);
        switched_here = (ng == 0);
    }
    status.cache_installed =
        (ntf_install_cache(status.ng_enabled ? shaper_ng : shaper_old) == 0);

    // The newer shaper leaves the justification class unset, and it is this detour that fills it
    // in. Switching the engine but failing to install the detour would therefore break justified
    // text with nothing to report it, so put the engine back rather than leave that combination
    // running. Only undone if this is what switched it: a firmware that ships the newer shaper on
    // keeps its own setting.
    if (!status.cache_installed && switched_here) {
        ntf_disable_harfbuzz_ng(shape_text);
        status.ng_enabled = false;
    }
    return status;
}

