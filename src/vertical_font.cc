#include "vertical_font.h"

#include <QtGui/private/qrawfont_p.h>
#include <QtCore/QList>
#include <QtCore/QSharedPointer>
#include <QtCore/QThreadStorage>

namespace {
typedef HB_Face (*NewFace)(void *, HB_GetFontTableFunc);
typedef HB_Face (*LoadFace)(HB_Face);
typedef void (*FreeFace)(HB_Face);
NewFace new_face = nullptr;
LoadFace load_face = nullptr;
FreeFace free_face = nullptr;
QFontEngineInterface **plugin = nullptr;

HB_Error font_table(void *font, HB_Tag tag, HB_Byte *buffer, HB_UInt *length)
{
    QFontEngine *engine = static_cast<QFontEngine *>(font);
    return engine->getSfntTableData(tag, buffer, length) ? HB_Err_Ok : HB_Err_Invalid_Argument;
}

struct Face {
    explicit Face(const QRawFont &source) : font(source), face(nullptr) {}
    ~Face() { if (face) free_face(face); }
    // The legacy table callback borrows the engine. Keep it alive until after qHBFreeFace.
    QRawFont font;
    HB_Face face;
};
typedef QSharedPointer<Face> FaceRef;
typedef QList<FaceRef> Faces;
const int max_faces = 8;
// QRawFont and its engine belong to their creating thread. A per-thread LRU also keeps the
// legacy extension's mutable shaping buffers from being shared across threads.
QThreadStorage<Faces *> caches;

FaceRef face_for(const QRawFont &font)
{
    if (!new_face || !load_face || !free_face || !plugin || !*plugin || !font.isValid())
        return FaceRef();
    QFontEngine *engine = QRawFontPrivate::get(font)->fontEngine;
    if (!caches.hasLocalData()) caches.setLocalData(new Faces);
    Faces &faces = *caches.localData();
    for (int i = 0; i < faces.size(); ++i) {
        if (QRawFontPrivate::get(faces[i]->font)->fontEngine != engine) continue;
        FaceRef result = faces.takeAt(i);
        faces.prepend(result);
        return result;
    }
    FaceRef result(new Face(font));
    result->face = new_face(engine, font_table);
    if (!result->face) return FaceRef();
    if (result->face->font_for_init) result->face = load_face(result->face);
    if (!result->face) return FaceRef();
    result->face->isSymbolFont = engine->symbol;
    faces.prepend(result);
    if (faces.size() > max_faces) faces.removeLast();
    return result;
}
} // namespace

bool ntf_vertical_font_prepare(void *create, void *load, void *release, void *interface)
{
    if (!create || !load || !release || !interface) return false;
    new_face = reinterpret_cast<NewFace>(create);
    load_face = reinterpret_cast<LoadFace>(load);
    free_face = reinterpret_cast<FreeFace>(release);
    plugin = static_cast<QFontEngineInterface **>(interface);
    return true;
}

bool ntf_vertical_font_has_glyphs(const QRawFont &font)
{
    try {
        const FaceRef face = face_for(font);
        return face && (*plugin)->hasVerticalGlyphs(face->face);
    } catch (...) {
        return false;
    }
}

unsigned ntf_vertical_font_substitute(const QRawFont &font, unsigned *glyphs, unsigned count)
{
    if (!glyphs || !count) return 0;
    const unsigned first = glyphs[0];
    try {
        const FaceRef face = face_for(font);
        if (face) return (*plugin)->substituteWithVerticalVariants(face->face, glyphs, count);
    } catch (...) {
        // Never fall through to the original adapter while NG owns the engine's face slot:
        // the vertical extension would interpret hb_face_t as the unrelated HB_FaceRec.
    }
    return first;
}
