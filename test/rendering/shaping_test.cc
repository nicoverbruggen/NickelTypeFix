// Include production sources to count shaper calls, disable cache recording, and check the
// small-caps mapping buffer directly. No test counters enter the shipped mod.
#include "../../src/shape_cache.cc"
#include "../../src/small_caps.cc"
#include "qt_runtime.h"
#include <QtWidgets/QApplication>
#include <QtGui/QFontDatabase>
#include <QtGui/QGlyphRun>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QTextLayout>
#include <QtGui/private/qrawfont_p.h>
#include <vector>

static void check(bool condition, const char *message)
{
    if (!condition) qFatal("FAIL: %s", message);
}

extern "C" __attribute__((noreturn)) void ntf_patch_unsafe(const char *reason)
{
    qFatal("FAIL: detour could not recover: %s", reason);
}

static ShapeFn originalShape;
static unsigned shapeCalls;
static int counted_shape(const QTextEngine *engine, const QScriptItem &item,
                          const ushort *text, int length, QFontEngine *font,
                          const QVector<uint> &boundaries, bool kerning)
{
    ++shapeCalls;
    return originalShape(engine, item, text, length, font, boundaries, kerning);
}

struct Run {
    QVector<quint32> glyphs;
    QVector<QPointF> positions;
    QString family;
    qreal size;
    bool rtl;
    bool operator==(const Run &other) const {
        return glyphs == other.glyphs && positions == other.positions && family == other.family &&
               size == other.size && rtl == other.rtl;
    }
};

struct Snapshot {
    std::vector<Run> runs;
    std::vector<qreal> engineSizes;
    QVector<unsigned> justification;
    QVector<ushort> clusters;
    QImage pixels;
    qreal width, ascent, descent;
    bool operator==(const Snapshot &other) const {
        return runs == other.runs && engineSizes == other.engineSizes &&
               justification == other.justification && clusters == other.clusters &&
               pixels == other.pixels && width == other.width &&
               ascent == other.ascent && descent == other.descent;
    }
};

static Snapshot render(const QString &text, const QFont &font, Qt::LayoutDirection direction = Qt::LeftToRight,
                       bool designMetrics = false, qreal origin = 4)
{
    QTextLayout layout(text, font);
    QTextOption option;
    option.setTextDirection(direction);
    option.setUseDesignMetrics(designMetrics);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    check(line.isValid(), "no text line");
    line.setLineWidth(2400);
    layout.endLayout();
    Snapshot result;
    result.width = line.naturalTextWidth();
    result.ascent = line.ascent(); result.descent = line.descent();
    for (const QGlyphRun &run : layout.glyphRuns()) {
        Run r = {run.glyphIndexes(), run.positions(), run.rawFont().familyName(),
                 run.rawFont().pixelSize(), run.isRightToLeft()};
        for (quint32 glyph : r.glyphs) check(glyph != 0, "fixture rendered a missing glyph");
        result.runs.push_back(r);
    }
    check(!result.runs.empty(), "no glyph runs");
    // Qt reconstructs QGlyphRun::rawFont from its QFont, bypassing the fontEngine detour.
    // Check the actual engines used by shaping and QTextLayout::draw separately.
    QTextEngine *engine = layout.engine();
    for (int i = 0; i < engine->layoutData->items.size(); ++i) {
        const QScriptItem &item = engine->layoutData->items[i];
        result.engineSizes.push_back(engine->fontEngine(item)->fontDef.pixelSize);
        const QGlyphLayout glyphs = engine->availableGlyphs(&item);
        for (int g = 0; g < item.num_glyphs; ++g)
            result.justification.append(glyphs.attributes[g].justification);
        const ushort *clusters = engine->logClusters(&item);
        for (int c = 0; c < engine->length(i); ++c) result.clusters.append(clusters[c]);
    }
    result.pixels = QImage(2400, 140, QImage::Format_ARGB32);
    result.pixels.fill(Qt::white);
    QPainter painter(&result.pixels);
    layout.draw(&painter, QPointF(origin, 4));
    return result;
}

static QFont font(const char *family, int size = 36)
{
    QFont f(QString::fromLatin1(family));
    f.setPixelSize(size);
    return f;
}


static QImage ink(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixel(x, y) != qRgb(255, 255, 255)) bounds |= QRect(x, y, 1, 1);
    check(!bounds.isEmpty(), "empty spacing fixture");
    return image.copy(bounds);
}

static void ng_spacing_is_independent_of_origin(bool ng)
{
    if (!ng) return;
    ntf_cache_records = false;
    bool reproduced = false;
    for (int size : {22, 26, 31}) {
        QFont f = font("Vollkorn", size);
        const QString text = QStringLiteral("she slowly AVATAR");
        ntf_shaper_is_ng = false;
        const QImage original = ink(render(text, f).pixels);
        for (qreal origin : {4.25, 4.5, 4.75})
            reproduced |= ink(render(text, f, Qt::LeftToRight, false, origin).pixels) != original;
        ntf_shaper_is_ng = true;
        const QImage expected = ink(render(text, f).pixels);
        for (qreal origin : {4.25, 4.5, 4.75})
            check(ink(render(text, f, Qt::LeftToRight, false, origin).pixels) == expected,
                  "NG word spacing changed at a fractional draw origin");
        ntf_cache_records = true;
        (void)render(text, f);
        const unsigned calls = shapeCalls;
        for (qreal origin : {4.25, 4.5, 4.75})
            check(ink(render(text, f, Qt::LeftToRight, false, origin).pixels) == expected,
                  "cached NG spacing changed at a fractional draw origin");
        check(shapeCalls == calls, "corrected spacing did not exercise cache replay");
        const QString longText = QString(100, QLatin1Char('n')) + text;
        const QImage longExpected = ink(render(longText, f).pixels);
        const unsigned long records = ntf_cache_stored;
        const unsigned longCalls = shapeCalls;
        check(ink(render(longText, f, Qt::LeftToRight, false, 4.25).pixels) == longExpected,
              "uncached long NG item changed spacing at a fractional draw origin");
        check(shapeCalls > longCalls && ntf_cache_stored == records,
              "long spacing fixture did not bypass cache recording");
        ntf_cache_records = false;
        ntf_shaper_is_ng = false;
        const Snapshot design = render(text, f, Qt::LeftToRight, true);
        ntf_shaper_is_ng = true;
        check(render(text, f, Qt::LeftToRight, true) == design, "NG correction changed design metrics");
        f.setKerning(false);
        ntf_shaper_is_ng = false;
        const Snapshot noKern = render(text, f);
        ntf_shaper_is_ng = true;
        check(render(text, f) == noKern, "NG correction changed unkerned text");
    }
    check(reproduced, "fixture did not reproduce fractional-origin spacing before correction");
    qDebug("PASS: NG spacing at four draw origins, design metrics, and kerning off");
}

static void ng_adjustment_rounding_guards(bool ng)
{
    if (!ng) return;
    const int adjustments[] = {-97, -96, -95, -33, -32, -31, 0, 31, 32, 33, 95, 96, 97};
    const int rounded[] = {-128, -64, -64, -64, 0, 0, 0, 0, 64, 64, 64, 128, 128};
    bool fractionalBase = false;
    for (bool unhinted : {false, true}) {
        QFont f = font("Vollkorn", 27);
        if (unhinted) f.setHintingPreference(QFont::PreferNoHinting);
        QTextLayout layout(QString(13, QLatin1Char('n')), f);
        layout.setCacheEnabled(true);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(2000);
        layout.endLayout();
        (void)line.naturalTextWidth();
        QTextEngine *e = layout.engine();
        QScriptItem &si = e->layoutData->items[0];
        check(si.num_glyphs == 13, "rounding fixture has unexpected glyph count");
        QFontEngine *fe = e->fontEngine(si);
        QGlyphLayout g = e->availableGlyphs(&si).mid(0, 13);
        QVarLengthGlyphLayoutArray base(13);
        memcpy(base.glyphs, g.glyphs, 13 * sizeof(glyph_t));
        fe->recalcAdvances(&base, QFontEngine::ShaperFlags(0));
        for (int i = 0; i < 13; i++) {
            fractionalBase |= (base.advances_x[i].value() & 63) != 0;
            g.advances_x[i] = base.advances_x[i] + QFixed::fromFixed(adjustments[i]);
        }
        ntf_round_ng_adjustments(e, si, fe, 13);
        for (int i = 0; i < 13; i++)
            check(g.advances_x[i] == base.advances_x[i] + QFixed::fromFixed(rounded[i]),
                  "NG adjustment rounding changed base metrics or signed-half convention");
        for (int guard = 0; guard < 7; guard++) {
            for (int i = 0; i < 13; i++) g.advances_x[i] = base.advances_x[i] + QFixed::fromFixed(17);
            if (guard == 0) e->option.setUseDesignMetrics(true);
            if (guard == 1) si.analysis.bidiLevel = 1;
            if (guard == 2) g.offsets[5].x = QFixed::fromFixed(17);
            if (guard == 3) g.offsets[5].y = QFixed::fromFixed(17);
            if (guard == 4) g.advances_y[5] = 1;
            if (guard == 5) g.advances_x[5] = 0;
            if (guard == 6) ntf_shaper_is_ng = false;
            QVector<QFixed> before;
            for (int i = 0; i < 13; i++) before.append(g.advances_x[i]);
            ntf_round_ng_adjustments(e, si, fe, 13);
            for (int i = 0; i < 13; i++)
                check(g.advances_x[i] == before[i], "NG correction ignored a positioning guard");
            check(memcmp(g.glyphs, base.glyphs, 13 * sizeof(glyph_t)) == 0, "NG correction changed glyph IDs");
            e->option.setUseDesignMetrics(false);
            si.analysis.bidiLevel = 0;
            g.offsets[5].x = 0;
            g.offsets[5].y = 0;
            g.advances_y[5] = 0;
            ntf_shaper_is_ng = true;
        }
    }
    check(fractionalBase, "rounding fixture did not exercise fractional base metrics");
    qDebug("PASS: signed GPOS rounding preserves fractional bases and guarded positioning");
}


// The minimal offscreen font database does not discover fallback families. Route two
// real loaded engines explicitly, using the same encoded glyph IDs as Qt's fallback path.
class RoundingFontEngineMulti : public QFontEngineMulti
{
public:
    RoundingFontEngineMulti(QFontEngine *primary, QFontEngine *fallback) : QFontEngineMulti(2)
    {
        engines[0] = primary;
        engines[1] = fallback;
        primary->ref.ref();
        fallback->ref.ref();
        fontDef = primary->fontDef;
    }
    void loadEngine(int) override { qFatal("unexpected fallback engine request"); }
};

static void ng_rounding_preserves_fallback_glyphs(bool ng)
{
    if (!ng) return;
    QFont f = font("Vollkorn", 27);
    QRawFont primary = QRawFont::fromFont(f);
    QRawFont fallback = QRawFont::fromFont(font("DejaVu Sans", 41));
    QFontEngine *engines[] = {QRawFontPrivate::get(primary)->fontEngine,
                            QRawFontPrivate::get(fallback)->fontEngine};
    RoundingFontEngineMulti multi(engines[0], engines[1]);
    const glyph_t ids[] = {primary.glyphIndexesForString(QStringLiteral("n"))[0],
                          fallback.glyphIndexesForString(QStringLiteral("m"))[0]};
    check(ids[0] && ids[1], "fallback fixture has a missing glyph");
    QTextLayout layout(QStringLiteral("nnn"), f);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(2000);
    layout.endLayout();
    (void)line.naturalTextWidth();
    QTextEngine *e = layout.engine();
    const QScriptItem &si = e->layoutData->items[0];
    check(si.num_glyphs == 3, "fallback fixture has unexpected glyph count");
    QGlyphLayout g = e->availableGlyphs(&si).mid(0, 3);
    QFixed expected[3];
    glyph_t encoded[3];
    for (int i = 0; i < 3; ++i) {
        const int which = i == 1 ? 1 : 0;
        QVarLengthGlyphLayoutArray single(1);
        single.glyphs[0] = ids[which];
        engines[which]->recalcAdvances(&single, QFontEngine::ShaperFlags(0));
        expected[i] = single.advances_x[0];
        encoded[i] = (which << 24) | ids[which];
        g.glyphs[i] = encoded[i];
        g.advances_x[i] = expected[i] + QFixed::fromFixed(17);
    }
    check(expected[0] != expected[1], "fallback fixture did not use distinct base metrics");
    ntf_round_ng_adjustments(e, si, &multi, 3);
    for (int i = 0; i < 3; ++i) {
        check(g.glyphs[i] == encoded[i], "NG rounding changed encoded fallback glyphs");
        check(g.advances_x[i] == expected[i], "NG rounding used the wrong fallback metrics");
    }
    qDebug("PASS: NG rounding routes encoded fallback IDs to two real font engines");
}

static void cache_matches_original()
{
    struct Case { QString text; QFont font; Qt::LayoutDirection direction; };
    QFont noKern = font("Vollkorn"); noKern.setKerning(false);
    QFont bold = font("Noto Sans"); bold.setBold(true);
    const Case cases[] = {
        {QStringLiteral("office affix AVATAR"), font("Vollkorn"), Qt::LeftToRight},
        {QStringLiteral("office affix AVATAR"), noKern, Qt::LeftToRight},
        {QStringLiteral("office affix AVATAR"), font("Noto Sans"), Qt::LeftToRight},
        {QStringLiteral("office affix AVATAR"), font("Noto Sans", 27), Qt::LeftToRight},
        {QStringLiteral("office affix AVATAR"), bold, Qt::LeftToRight},
        {QString::fromUtf8("café a\xcc\x81 naïve Ελληνικά"), font("Noto Sans"), Qt::LeftToRight},
        {QString::fromUtf8("مرحبا بالعالم"), font("Noto Sans Arabic"), Qt::RightToLeft},
        {QString(96, QLatin1Char('n')), font("Noto Sans", 18), Qt::LeftToRight},
        {QString(97, QLatin1Char('n')), font("Noto Sans", 18), Qt::LeftToRight}
    };
    unsigned firstCalls = 0, repeatedCalls = 0;
    for (const Case &c : cases) {
        ntf_cache_records = false;
        Snapshot expected = render(c.text, c.font, c.direction);
        ntf_cache_records = true;
        unsigned long records = ntf_cache_stored;
        unsigned before = shapeCalls;
        Snapshot first = render(c.text, c.font, c.direction);
        unsigned firstCount = shapeCalls - before;
        firstCalls += firstCount;
        before = shapeCalls;
        Snapshot repeated = render(c.text, c.font, c.direction);
        unsigned repeatCount = shapeCalls - before;
        repeatedCalls += repeatCount;
        check(first == expected, "cache miss changed glyphs, positions, metrics, or pixels");
        check(repeated == expected, "cache replay changed glyphs, positions, metrics, or pixels");
        if (c.text.size() == 96)
            check(ntf_cache_stored > records && repeatCount == 0, "96-character item was not cached");
        if (c.text.size() == 97)
            check(ntf_cache_stored == records && firstCount > 0 && repeatCount > 0,
                  "97-character item did not bypass recording");
    }
    check(ntf_cache_stored > 0 && firstCalls > repeatedCalls, "cache test did not exercise a replay");
    qDebug("PASS: cache records=%lu, original calls first=%u repeated=%u", ntf_cache_stored, firstCalls, repeatedCalls);
}

static void cache_separates_metric_modes()
{
    // Qt callers can request fractional design advances or pixel metrics with the same font. Seed each mode first, then ask the cache for the other one.
    for (bool firstDesign : {false, true}) {
        QFont f = font("Vollkorn", firstDesign ? 23 : 22);
        const QString text = QStringLiteral("she slowly she AVATAR");
        ntf_cache_records = false;
        const Snapshot first = render(text, f, Qt::LeftToRight, firstDesign);
        const Snapshot second = render(text, f, Qt::LeftToRight, !firstDesign);
        check(!(first == second), "metric modes do not differ in the fixture");
        ntf_cache_records = true;
        check(render(text, f, Qt::LeftToRight, firstDesign) == first,
              "first metric mode changed while recording");
        check(render(text, f, Qt::LeftToRight, !firstDesign) == second,
              "cache reused advances from the other metric mode");
        const unsigned before = shapeCalls;
        check(render(text, f, Qt::LeftToRight, firstDesign) == first &&
              render(text, f, Qt::LeftToRight, !firstDesign) == second,
              "metric mode replay changed rendering");
        check(shapeCalls == before, "metric mode test did not replay both records");
    }
    qDebug("PASS: design and device metrics remain separate in both cache orders");
}

static QVector<quint32> glyphs(const Snapshot &snapshot)
{
    QVector<quint32> result;
    for (const Run &run : snapshot.runs) result += run.glyphs;
    return result;
}

static void primary_mapping_preserves_fallbacks()
{
    const QRawFont raw = QRawFont::fromFont(font("Vollkorn"));
    check(raw.isValid(), "primary mapping fixture did not load");
    QFontEngine *primary = QRawFontPrivate::get(raw)->fontEngine;
    const QString text = QStringLiteral("ofe");
    const QVector<quint32> expected = raw.glyphIndexesForString(text);
    QByteArray storage(QGlyphLayout::spaceNeededForGlyphLayout(4), 0);
    QGlyphLayout prepared(storage.data(), 4);
    // Qt has prepared capitals, with the middle glyph assigned to another font.
    prepared.glyphs[0] = 138;
    prepared.glyphs[1] = 0x0100004b;
    prepared.glyphs[2] = 48;
    prepared.glyphs[3] = 0x12345678;
    const QByteArray before(storage.constData(), storage.size());
    prepared.numGlyphs = 2;
    check(!ntf_sc_map_primary(prepared, text.utf16(), text.size(), primary), "short glyph buffer accepted");
    check(storage == before, "failed mapping changed Qt's prepared buffer");
    prepared.numGlyphs = 4;
    check(ntf_sc_map_primary(prepared, text.utf16(), text.size(), primary), "primary mapping failed");
    check(prepared.glyphs[0] == expected[0] && prepared.glyphs[2] == expected[2],
          "primary glyphs were not mapped from lowercase text");
    check(prepared.glyphs[1] == 0x0100004b && prepared.glyphs[3] == 0x12345678,
          "primary mapping changed a fallback glyph or the buffer tail");
    const QString missing(QChar(0x0180));
    check(!raw.supportsCharacter(uint(0x0180)), "missing-letter fixture changed");
    const glyph_t previous = prepared.glyphs[0];
    check(ntf_sc_map_primary(prepared, missing.utf16(), 1, primary), "missing-letter mapping failed");
    check(prepared.glyphs[0] == previous, "missing lowercase replaced an existing glyph with .notdef");
    qDebug("PASS: primary mapping preserves fallback IDs, missing letters, and short buffers");
}

static int small_caps_use_font_glyphs()
{
    primary_mapping_preserves_fallbacks();
    QFont supported = font("Vollkorn"); supported.setCapitalization(QFont::SmallCaps);
    QFont unsupported = font("DejaVu Sans"); unsupported.setCapitalization(QFont::SmallCaps);
    const QString text = QStringLiteral("office");
    Snapshot oldSupported = render(text, supported);
    Snapshot oldUnsupported = render(text, unsupported);
    Snapshot plain = render(text, font("Vollkorn"));
    void *entry = dlsym(RTLD_DEFAULT, "_ZNK11QTextEngine10fontEngineERK11QScriptItemP6QFixedS4_S4_");
    check(ntf_smallcaps_enable(entry, true, [](const char *line) { qDebug("%s", line); }).installed,
          "fontEngine detour did not install");
    // Earlier runs contain stock small caps records. Start this phase without those records.
    // Production installs both detours before any text is shaped.
    ntf_cache_records = false;
    Snapshot actual = render(text, supported);
    // These IDs come from the pinned font's post table names o.sc, f.sc, i.sc, c.sc and e.sc.
    // The expectation is independent of the production GSUB parser.
    const QVector<quint32> expected = {797, 725, 725, 742, 680, 696};
    for (qreal size : actual.engineSizes) check(size == 36, "small caps still use a scaled font engine");
    check(actual.ascent == oldSupported.ascent && actual.descent == oldSupported.descent,
          "small caps changed line ascent or descent");
    check(actual.pixels != oldSupported.pixels, "small caps fixture did not change the stock rendering");
    check(render(text, unsupported) == oldUnsupported, "font without smcp no longer uses stock small caps");
    check(render(text, font("Vollkorn")) == plain, "small caps detour changed ordinary text");

    // A fresh size avoids records made before the small-caps detour was installed.
    supported.setPixelSize(37);
    Snapshot reference = render(text, supported);
    ntf_cache_records = true;
    check(render(text, supported) == reference, "small caps cache miss changed rendering");
    unsigned before = shapeCalls;
    check(render(text, supported) == reference, "small caps cache replay changed rendering");
    check(shapeCalls == before, "small caps replay called the original shaper");
    check(glyphs(actual) == expected, "small caps did not use the expected font glyphs or expand ffi");
    qDebug("PASS: small-caps glyphs, ligature expansion, full-size engine, metrics, and cache replay");
    return 0;
}

static void small_caps_cache_cycles()
{
    const QRawFont raw = QRawFont::fromFont(font("Vollkorn"));
    check(raw.isValid(), "cache fixture did not load");
    QFontEngine *engine = QRawFontPrivate::get(raw)->fontEngine;
    const QString family = engine->fontDef.family;
    const quint32 lower = raw.glyphIndexesForString(QStringLiteral("o"))[0];
    // Give one real font distinct face identities. The cache reads real GSUB/GPOS tables,
    // but the test needs no collection of 65 font files. Restore the engine before rendering.
    auto lookup = [&](int face) {
        engine->fontDef.family = QStringLiteral("cache fixture %1").arg(face);
        const NtfScFontRef record = ntf_sc_font_for(engine);
        check(record && record->has_smcp, "small caps stopped working after the cache filled");
        check(ntf_sc_font_count <= NTF_SC_MAX_FONTS, "font cache exceeded its capacity");
        return record;
    };
    NtfScFontRef first = lookup(0);
    const uint16_t small = first->smcp[lower];
    check(small == 797, "wrong cached small-cap glyph");
    const QWeakPointer<const NtfScFont> firstLifetime(first);
    QWeakPointer<const NtfScFont> oldest;
    for (int i = 1; i < NTF_SC_MAX_FONTS; ++i) {
        const NtfScFontRef record = lookup(i);
        if (i == 1) oldest = record;
    }
    check(lookup(0).data() == first.data(), "cache hit rebuilt a font record");
    lookup(NTF_SC_MAX_FONTS);
    check(oldest.isNull(), "cache did not release the least recently used record");
    check(lookup(0).data() == first.data(), "cache evicted a recently used record");

    for (int i = NTF_SC_MAX_FONTS + 1; i <= 2 * NTF_SC_MAX_FONTS; ++i) lookup(i);
    bool firstCached = false;
    for (int i = 0; i < ntf_sc_font_count; ++i)
        if (ntf_sc_fonts[i].data() == first.data()) firstCached = true;
    check(!firstCached, "inactive record remained cached after a full cycle");
    check(!firstLifetime.isNull() && first->smcp[lower] == small && !first->ligatures.isEmpty(),
          "eviction invalidated a record still used by a render");
    first.clear();
    check(firstLifetime.isNull(), "evicted record survived its final reader");
    check(lookup(0)->smcp[lower] == small, "revisiting an evicted font changed its small caps");
    engine->fontDef.family = family;
    qDebug("PASS: 65 face identities, LRU eviction, held-record lifetime, and revisiting a font");
}

struct LineSnapshot {
    QVector<qreal> positions;
    QVector<int> cursors;
    QImage pixels;
    bool operator==(const LineSnapshot &other) const {
        return positions == other.positions && cursors == other.cursors && pixels == other.pixels;
    }
};

static LineSnapshot render_lines(const QString &text, Qt::LayoutDirection direction)
{
    QTextLayout layout(text, font("Noto Sans"));
    QTextOption option;
    option.setTextDirection(direction);
    option.setAlignment(Qt::AlignJustify);
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal y = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(260);
        line.setPosition(QPointF(7, y));
        y += line.height();
    }
    layout.endLayout();
    LineSnapshot result;
    for (int i = 0; i < layout.lineCount(); i++) {
        QTextLine line = layout.lineAt(i);
        result.positions << line.naturalTextWidth() << line.height();
        for (int pos = line.textStart(); pos <= line.textStart() + line.textLength(); pos++) {
            for (QTextLine::Edge edge : {QTextLine::Leading, QTextLine::Trailing}) {
                int cursor = pos;
                result.positions << line.cursorToX(&cursor, edge);
                result.cursors << cursor;
            }
        }
    }
    result.pixels = QImage(280, qMax(1, (int)y + 1), QImage::Format_ARGB32_Premultiplied);
    result.pixels.fill(Qt::white);
    QPainter painter(&result.pixels);
    layout.draw(&painter, QPointF());
    painter.end();
    return result;
}

static NtfShapeLineFn originalLine;
static unsigned lineCalls;
static void counted_line(QTextEngine *engine, const QScriptLine &line)
{
    ++lineCalls;
    originalLine(engine, line);
}

static void line_layout_matches_original()
{
    const QString fixtures[] = {
        QStringLiteral("A familiar office offers efficient filing. Another line follows it."),
        QString::fromUtf8("Cafe\xcc\x81, office\xc2\xa0" "files and co\xc2\xad" "operate."),
        QString::fromUtf8("English \xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9 123 English"),
        QStringLiteral("one\ttwo\tthree\nA second line with\ttabs."),
        QStringLiteral("An unbrokenwordlongenoughtowrapacrossmultiplelines finishes here."),
    };
    QVector<LineSnapshot> expected;
    for (const QString &text : fixtures)
        for (Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft})
            expected.append(render_lines(text, direction));

    check(!ntf_line_layout_enable(nullptr), "missing line-layout symbol did not sit out");
    check(ntf_line_layout_enable(dlsym(RTLD_DEFAULT, "_ZN11QTextEngine9shapeLineERK11QScriptLine")),
          "line-layout detour did not install");
    originalLine = ntf_original_shape_line;
    ntf_original_shape_line = counted_line;
    int i = 0;
    for (const QString &text : fixtures)
        for (Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft})
            check(render_lines(text, direction) == expected[i++],
                  "line shortcut changed cursor positions, metrics, or pixels");

    QTextEngine engine(QStringLiteral("one two three"), font("Noto Sans"));
    QScriptLine line;
    line.from = 0;
    line.length = engine.text.size();
    check(!ntf_line_layout_prepared(&engine, line), "unitemized line was treated as ready");
    engine.itemize();
    check(!ntf_line_layout_prepared(&engine, line), "unshaped line was treated as ready");
    unsigned before = lineCalls;
    ntf_prepare_line(&engine, line);
    check(lineCalls == before + 1, "unshaped line skipped the original preparation");
    check(ntf_line_layout_prepared(&engine, line), "shaped ordinary line was not ready");
    before = lineCalls;
    ntf_prepare_line(&engine, line);
    check(lineCalls == before, "ready line repeated its preparation");

    QScriptItem &item = engine.layoutData->items.last();
    const unsigned flags = item.analysis.flags;
    item.analysis.flags = QScriptAnalysis::Tab;
    check(!ntf_line_layout_prepared(&engine, line), "tab width calculation was skipped");
    item.analysis.flags = QScriptAnalysis::Object;
    check(!ntf_line_layout_prepared(&engine, line), "inline object resizing was skipped");
    item.analysis.flags = flags;
    item.num_glyphs = 0;
    check(!ntf_line_layout_prepared(&engine, line), "partially shaped line was treated as ready");
    before = lineCalls;
    render_lines(fixtures[3], Qt::LeftToRight);
    check(lineCalls > before, "tab fixture never reached the original line preparation");
    qDebug("PASS: line preparation, cursor positions, wrapping, bidi, tabs, and fallback guards");
}

int main(int argc, char **argv)
{
    check(argc == 3, "supply the fixture font directory and cache, small-caps, or line-layout");
    bool ng = qgetenv("QT_HARFBUZZ") == "ng";
    ntf_test_select_shaper(ng);
    QApplication app(argc, argv);
    check(qVersion() == QStringLiteral("5.2.1"), "use the pinned Qt 5.2.1 runtime");
    for (const char *name : {"Vollkorn-Regular.ttf", "NotoSans-Regular.ttf", "NotoSansArabic-Regular.ttf", "DejaVuSans.ttf"})
        check(QFontDatabase::addApplicationFont(QString::fromLocal8Bit(argv[1]) + '/' + name) >= 0,
              "fixture font did not load");
    const char *symbol = ng
        ? "_ZNK11QTextEngine23shapeTextWithHarfbuzzNGERK11QScriptItemPKtiP11QFontEngineRK7QVectorIjEb"
        : "_ZNK11QTextEngine21shapeTextWithHarfbuzzERK11QScriptItemPKtiP11QFontEngineRK7QVectorIjEb";
    // The selector was set before QApplication. Install the production cache on that shaper.
    check(ntf_install_cache(dlsym(RTLD_DEFAULT, symbol), ng) == 0, "shaper detour did not install");
    originalShape = ntf_original_shape;
    ntf_original_shape = counted_shape;
    if (strcmp(argv[2], "line-layout") == 0) {
        line_layout_matches_original();
        return 0;
    }
    if (strcmp(argv[2], "cache") == 0) {
        ng_spacing_is_independent_of_origin(ng);
        ng_adjustment_rounding_guards(ng);
        ng_rounding_preserves_fallback_glyphs(ng);
        cache_matches_original();
        cache_separates_metric_modes();
        return 0;
    }
    check(strcmp(argv[2], "small-caps") == 0, "unknown shaping test");
    small_caps_cache_cycles();
    return small_caps_use_font_glyphs();
}
