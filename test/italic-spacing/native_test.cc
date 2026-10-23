// Exercise the native production code against pinned ARM Qt. Fake WebKit objects
// supply the verified ABI fields; real Qt shapes, rasterizes and caches the glyphs.
#include "../../src/italic_spacing.cc"
#include "../../src/shape_cache.cc"
#include "../../src/small_caps.cc"
#include "../rendering/qt_runtime.h"
#include <QApplication>
#include <array>
#include <new>

static void check(bool value, const char *message) { if (!value) qFatal("FAIL: %s", message); }
extern "C" __attribute__((noreturn)) void ntf_patch_unsafe(const char *reason) { qFatal("FAIL: %s", reason); }
template <typename T> static void put(void *p, unsigned offset, T value) {
    std::memcpy(static_cast<char *>(p) + offset, &value, sizeof value);
}
struct Face {
    alignas(8) char inherited[144] = {}, glyphs[64] = {}, primary[64] = {}, platform[32] = {};
    Face(const QRawFont &font, bool slanted) {
        new (platform + 4) QRawFont(font);
        put(primary, 52, static_cast<void *>(platform));
        put(glyphs, 44, static_cast<void *>(primary));
        put(data(), 44, static_cast<void *>(glyphs));
        put(data(), 28, static_cast<unsigned char>(slanted ? 16 : 0));
    }
    ~Face() { reinterpret_cast<QRawFont *>(platform + 4)->~QRawFont(); }
    void *data() { return inherited + 16; }
};
struct Renderer {
    alignas(8) char object[64] = {}, style[64] = {}, surround[112] = {}, string[24] = {};
    QString value;
    Renderer(Face &face, const QString &text) : value(text) {
        put(object, 4, static_cast<void *>(style));
        put(object, 24, unsigned((1 << 8) | (1 << 10) | (1 << 12)));
        put(object, 48, static_cast<void *>(string));
        put(style, 16, static_cast<void *>(surround));
        put(style, 28, static_cast<void *>(face.inherited));
        put(style, 40, unsigned(3 << 17)); // TTNONE, not CAPITALIZE.
        put(string, 4, unsigned(value.size()));
        put(string, 8, value.constData());
    }
};
struct Sample {
    double width, first_cursor;
    QVector<quint32> glyphs;
    QVector<QPointF> positions;
    InkRun ink;
};
static void original_formats(void *, QTextLayout *, const Run *) { }
static Sample sample(const QRawFont &font, const QString &text, Renderer *renderer = nullptr, Face *face = nullptr) {
    QTextLayout layout(text, font);
    if (renderer) {
        Run run = {text.constData(), unsigned(text.size()), unsigned(text.size()), 0, 1, 0, 16, 0, nullptr};
        pending = {&run, renderer->object, face->data()};
        format(face->data(), &layout, &run);
    }
    layout.beginLayout();
    QTextLine line = layout.createLine(); line.setLineWidth(100000); layout.endLayout();
    Sample result;
    result.width = line.cursorToX(text.size()); result.first_cursor = line.cursorToX(1);
    const QGlyphRun run = line.glyphRuns().first();
    result.glyphs = run.glyphIndexes(); result.positions = run.positions();
    result.ink.count = qMin(4, result.glyphs.size());
    unsigned first = result.glyphs.size() - result.ink.count;
    double origin = first ? result.positions[first].x() : 0;
    for (unsigned i = 0; i < result.ink.count; ++i) {
        result.ink.glyphs[i] = result.glyphs[first+i];
        result.ink.positions[i] = QPointF(result.positions[first+i].x() - origin, 0);
    }
    result.ink.advance = result.width - origin;
    return result;
}
static unsigned fake_advance(void *iterator, int to, void *) {
    unsigned previous = field<unsigned>(iterator, 8);
    const Run *run = static_cast<const Run *>(pointer(iterator, 4));
    unsigned current = qMin(unsigned(to), run->length);
    put(iterator, 8, current);
    return current - previous;
}
static unsigned cache_calls;
static float cache_value;
static float *fake_cache(void *, const Run *, float) { ++cache_calls; return &cache_value; }

int main(int argc, char **argv) {
    check(argc == 2, "supply the fixture font file");
    const bool ng = qgetenv("QT_HARFBUZZ") == "ng";
    ntf_test_select_shaper(ng);
    QApplication app(argc, argv);
    const char *symbol = ng
        ? "_ZNK11QTextEngine23shapeTextWithHarfbuzzNGERK11QScriptItemPKtiP11QFontEngineRK7QVectorIjEb"
        : "_ZNK11QTextEngine21shapeTextWithHarfbuzzERK11QScriptItemPKtiP11QFontEngineRK7QVectorIjEb";
    check(ntf_install_cache(dlsym(RTLD_DEFAULT, symbol), ng) == 0, "install production shaper cache");
    ntf_shape_postprocess([](const void *e, const void *item, int length, int count) {
        ntf_italic_native_adjust(static_cast<const QTextEngine *>(e), *static_cast<const QScriptItem *>(item), length, count);
    });
    active = true; reader_scope = [](const void *) { return 1.0; };
    original_format = original_formats; original_advance = fake_advance; original_width_cache = fake_cache;
    QRawFont raw(QString::fromLocal8Bit(argv[1]), 45, QFont::PreferNoHinting);
    check(raw.isValid(), "load the pinned font");
    Face left_face(raw, true), right_face(raw, false);
    Renderer left(left_face, QStringLiteral("AV")), right(right_face, QStringLiteral("’s"));
    put(left.object, 20, static_cast<void *>(right.object));

    Mask a, b;
    a.left.fill(100000, 5); a.right.fill(-100000, 5);
    b.left.fill(100000, 5); b.right.fill(-100000, 5);
    a.right[1] = 8; b.left[3] = 9;
    check(clearance(a, b, 1) == 1, "diagonal ink needs vertical clearance");
    b.left[3] = 12;
    check(clearance(a, b, 1) == 0, "already separated ink must keep its advance");

    Sample plain = sample(raw, left.value);
    const quint32 punctuation_glyph = raw.glyphIndexesForString(QStringLiteral("’")).first();
    cache.append({raw, raw, plain.ink, punctuation_glyph, 1, 3});
    Sample fixed = sample(raw, left.value, &left, &left_face);
    check(qAbs(fixed.width - plain.width - 3) < .001, "complex layout lost the boundary advance");
    check(fixed.glyphs == plain.glyphs && fixed.positions == plain.positions,
          "boundary correction changed shaping or kerning inside the word");
    check(fixed.first_cursor == plain.first_cursor, "interior cursor moved");
    check(qAbs(sample(raw, left.value).width - plain.width) < .001,
          "context-specific advance polluted the shared shaping cache");
    check(qAbs(sample(raw, left.value, &left, &left_face).width - fixed.width) < .001,
          "cache replay applied the boundary advance twice");
    ntf_cache_records = false;
    check(qAbs(sample(raw, left.value, &left, &left_face).width - fixed.width) < .001,
          "native correction depends on shaped-result caching");
    ntf_cache_records = true;

    Run run = {left.value.constData(), 2, 2, 0, 1, 0, 16, 0, nullptr};
    alignas(8) char iterator[128] = {}, buffer[50000] = {};
    quint32 ids[4] = {}; QPointF advances[4]; void *faces[4] = {};
    InkRun simple = plain.ink;
    const double origin = simple.positions[0].x();
    for (unsigned i = 0; i < simple.count; ++i) simple.positions[i].rx() -= origin;
    simple.advance -= origin;
    cache.append({raw, raw, simple, punctuation_glyph, 1, 3});
    for (unsigned i = 0; i < plain.ink.count; ++i) {
        ids[i] = plain.ink.glyphs[i]; faces[i] = left_face.primary;
        advances[i].setX((i+1 < simple.count ? simple.positions[i+1].x() : simple.advance) - simple.positions[i].x());
    }
    put(buffer, 0, faces); put(buffer, 8204, ids); put(buffer, 16408, advances);
    put(buffer, 8200, plain.ink.count); put(buffer, 16404, plain.ink.count); put(buffer, 49184, plain.ink.count);
    put(iterator, 0, left_face.data()); put(iterator, 4, &run); put(iterator, 12, float(simple.advance));
    remember(&run, left.object, left_face.data());
    check(pending.run == &run, "native run did not retain its renderer context");
    double last_advance = advances[plain.ink.count-1].x();
    advance(iterator, 2, buffer);
    check(qAbs(field<float>(iterator, 12) - simple.advance - 3) < .001, "simple layout lost the boundary advance");
    check(qAbs(advances[plain.ink.count-1].x() - last_advance - 3) < .001, "painted glyph buffer disagrees with layout");
    advance(iterator, 2, buffer);
    check(qAbs(field<float>(iterator, 12) - simple.advance - 3) < .001, "repeated advance added spacing twice");

    Run other = run;
    pending = {&run, left.object, left_face.data()};
    check(width_cache(nullptr, &run, 0) == nullptr, "WebKit width cache accepted context-specific width");
    check(width_cache(nullptr, &other, 0) == &cache_value && cache_calls == 1,
          "ordinary widths stopped using WebKit's cache");
    put(left.surround, 76, float(2));
    check(sample(raw, left.value, &left, &left_face).width == plain.width, "author padding changed");
    put(left.surround, 76, unsigned(0)); put(left.surround, 44, float(2));
    check(sample(raw, left.value, &left, &left_face).width == plain.width, "author margin changed");
    put(left.surround, 44, unsigned(0)); put(left.style, 48, unsigned(3 << 16));
    check(sample(raw, left.value, &left, &left_face).width == plain.width, "raised text changed");
    put(left.style, 48, unsigned(0)); put(right_face.data(), 28, static_cast<unsigned char>(16));
    check(sample(raw, left.value, &left, &left_face).width == plain.width, "all-italic text gained spacing");
    put(right_face.data(), 28, static_cast<unsigned char>(0));
    reader_scope = [](const void *) { return 0.0; };
    check(sample(raw, left.value, &left, &left_face).width == plain.width, "another document was changed");
    reader_scope = [](const void *) { return 1.0; };
    QRawFont larger = raw; larger.setPixelSize(60);
    size_t count = cache.size();
    correction(larger, larger, plain.ink, QChar(0x2019), 1);
    check(cache.size() == int(count+1) && cache.last().left.pixelSize() == 60,
          "font size reused a stale clearance measurement");
    for (unsigned i = 0; i < 160; ++i) {
        InkRun unique = plain.ink; unique.advance += i / 16.0;
        correction(raw, raw, unique, QChar(0x2019), 1);
    }
    check(cache.size() <= 128, "native measurement cache is unbounded");
    ntf_italic_native_reset();
    active = false;
    ntf_shape_postprocess(nullptr);
    qDebug("PASS: native glyph clearance, simple/complex advances, kerning, cursors, caches, author spacing, scope and font sizes");
}
