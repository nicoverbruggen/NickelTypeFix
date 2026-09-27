// Exercise the bridge with Qt's real font engines and legacy HarfBuzz. The plugin double
// rejects NG faces and checks the table callback independently against QRawFont::fontTable.
#include "../../src/vertical_font.cc"
#include "qt_runtime.h"
#include <QtWidgets/QApplication>
#include <QtCore/QThread>
#include <cstdio>

static void check(bool condition, const char *message)
{
    if (!condition) qFatal("FAIL: %s", message);
}

static unsigned created, freed;
static QByteArray expectedTable;
static HB_Face counted_new(void *font, HB_GetFontTableFunc table)
{
    HB_UInt length = 0;
    check(table(font, HB_MAKE_TAG('h','e','a','d'), nullptr, &length) == HB_Err_Ok,
          "table length callback failed");
    QByteArray actual(length, '\0');
    check(table(font, HB_MAKE_TAG('h','e','a','d'),
                reinterpret_cast<HB_Byte *>(actual.data()), &length) == HB_Err_Ok,
          "table callback failed");
    check(actual == expectedTable, "callback returned different font data");
    ++created;
    return qHBNewFace(font, table);
}
static void counted_free(HB_Face face)
{
    ++freed;
    qHBFreeFace(face);
}
static HB_Face failed_new(void *, HB_GetFontTableFunc) { return nullptr; }

class VerticalPlugin : public QFontEngineInterface {
public:
    QRawFont expected;
    HB_Face last = nullptr;
    bool hasVerticalGlyphs(HB_Face face) override {
        QFontEngine *engine = QRawFontPrivate::get(expected)->fontEngine;
        check(face != engine->face_, "legacy bridge must not borrow the engine's face slot");
        check(face->font_for_init == nullptr, "legacy face must be loaded");
        check(face->gsub != nullptr, "legacy face must contain parsed GSUB");
        last = face;
        return true;
    }
    quint32 substituteWithVerticalVariants(HB_Face face, quint32 *glyphs, unsigned count) override {
        check(hasVerticalGlyphs(face), "substitution needs the same valid legacy face");
        for (unsigned i = 0; i < count; ++i) glyphs[i] += 7;
        return glyphs[0];
    }
};

class Worker : public QThread {
public:
    QString path;
    VerticalPlugin *extension;
    void run() override {
        QRawFont font(path, 24);
        extension->expected = font;
        check(ntf_vertical_font_has_glyphs(font), "worker must own a separate legacy face");
        extension->expected = QRawFont();
    }
};

int main(int argc, char **argv)
{
    check(argc == 2, "expected font path");
    ntf_test_select_shaper(qgetenv("QT_HARFBUZZ") == "ng");
    QApplication app(argc, argv);
    VerticalPlugin extension;
    QFontEngineInterface *interface = &extension;
    check(!ntf_vertical_font_prepare(nullptr, (void *)qHBLoadFace, (void *)counted_free, &interface),
          "missing dependency must block NG");
    check(ntf_vertical_font_prepare((void *)counted_new, (void *)qHBLoadFace,
                                   (void *)counted_free, &interface), "prepare failed");
    QRawFont font(QString::fromLocal8Bit(argv[1]), 24);
    check(font.isValid(), "test font failed to load");
    extension.expected = font;
    expectedTable = font.fontTable("head");
    QFontEngine *engine = QRawFontPrivate::get(font)->fontEngine;
    void *qtFace = engine->harfbuzzFace();
    check(qtFace, "Qt must create its own face first");
    const QVector<quint32> glyphsBefore = font.glyphIndexesForString("Vertical glyph bridge");
    check(ntf_vertical_font_has_glyphs(font), "vertical glyph check failed");
    HB_Face legacy = extension.last;
    unsigned glyphs[] = {12, 23};
    check(ntf_vertical_font_substitute(font, glyphs, 2) == 19 && glyphs[0] == 19 && glyphs[1] == 30,
          "plugin result or glyph changes lost");
    check(extension.last == legacy && created == 1, "same engine must reuse its legacy face");
    check(engine->face_ == qtFace && font.glyphIndexesForString("Vertical glyph bridge") == glyphsBefore,
          "vertical calls must preserve Qt's face and horizontal glyph mapping");
    check(!ntf_vertical_font_has_glyphs(QRawFont()), "invalid font must decline");
    check(ntf_vertical_font_substitute(font, nullptr, 1) == 0 &&
          ntf_vertical_font_substitute(font, glyphs, 0) == 0, "empty input must not reach plugin");
    interface = nullptr;
    check(!ntf_vertical_font_has_glyphs(font) && ntf_vertical_font_substitute(font, glyphs, 2) == glyphs[0],
          "unloaded plugin must leave glyphs alone");
    interface = &extension;
    for (int size = 25; size < 45; ++size) {
        QRawFont other(QString::fromLocal8Bit(argv[1]), size);
        extension.expected = other;
        check(ntf_vertical_font_has_glyphs(other), "cache eviction lost a font");
    }
    check(created == 21 && created - freed == 8, "legacy face cache must remain bounded");
    extension.expected = font;
    check(ntf_vertical_font_has_glyphs(font) && created == 22, "evicted face must be recreated");
    check(engine->face_ == qtFace, "eviction must preserve Qt's live face");
    const unsigned beforeThread = created;
    Worker worker;
    worker.path = QString::fromLocal8Bit(argv[1]);
    worker.extension = &extension;
    worker.start();
    check(worker.wait(10000), "worker did not finish");
    check(created == beforeThread + 1 && created - freed == 8,
          "thread exit must release its own cache");
    extension.expected = QRawFont();
    font = QRawFont();
    caches.setLocalData(nullptr);
    check(created == freed, "cache teardown must free every legacy face");
    check(ntf_vertical_font_prepare((void *)failed_new, (void *)qHBLoadFace,
                                   (void *)counted_free, &interface), "failure setup failed");
    font = QRawFont(QString::fromLocal8Bit(argv[1]), 24);
    check(!ntf_vertical_font_has_glyphs(font) && ntf_vertical_font_substitute(font, glyphs, 2) == glyphs[0],
          "allocation failure must leave glyphs alone");
    std::puts("PASS: vertical legacy faces, NG isolation, font tables, eviction, teardown and failure handling");
}
