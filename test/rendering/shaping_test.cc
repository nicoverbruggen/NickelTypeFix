// Include the production cache so the test can count calls through its original trampoline
// and switch recording off for the reference run. No test counters enter the shipped mod.
#include "../../src/shape_cache.cc"
#include "qt_runtime.h"
#include <QtWidgets/QApplication>
#include <QtGui/QFontDatabase>
#include <QtGui/QGlyphRun>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QTextLayout>
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

static int small_caps_use_font_glyphs(bool ng)
{
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
    if (glyphs(actual) != expected) {
        // Known production bug: old HarfBuzz reuses Qt's uppercase glyph buffer for a
        // multi-font engine. Match that exact failure; any other mismatch is an error.
        if (!ng && glyphs(actual) == glyphs(oldSupported)) {
            qWarning("KNOWN FAILURE: old HarfBuzz retains uppercase glyph IDs for small caps");
            return 42;
        }
        qFatal("FAIL: small caps did not use the expected font glyphs or expand ffi");
    }
    qDebug("PASS: small-caps glyphs, ligature expansion, full-size engine, metrics, and cache replay");
    return 0;
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
    return small_caps_use_font_glyphs(ng);
}
