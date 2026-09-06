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

static Snapshot render(const QString &text, const QFont &font, Qt::LayoutDirection direction = Qt::LeftToRight)
{
    QTextLayout layout(text, font);
    QTextOption option;
    option.setTextDirection(direction);
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
    layout.draw(&painter, QPointF(4, 4));
    return result;
}

static QFont font(const char *family, int size = 36)
{
    QFont f(QString::fromLatin1(family));
    f.setPixelSize(size);
    return f;
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

int main(int argc, char **argv)
{
    check(argc == 3, "supply the fixture font directory and cache or small-caps");
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
    check(ntf_install_cache(dlsym(RTLD_DEFAULT, symbol)) == 0, "shaper detour did not install");
    originalShape = ntf_original_shape;
    ntf_original_shape = counted_shape;
    if (strcmp(argv[2], "cache") == 0) {
        cache_matches_original();
        return 0;
    }
    check(strcmp(argv[2], "small-caps") == 0, "unknown shaping test");
    small_caps_cache_cycles();
    return small_caps_use_font_glyphs();
}
