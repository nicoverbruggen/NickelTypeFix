#include "italic_spacing.h"
#include "detour.h"
#include "shape_cache.h"
#define private public
#define protected public
#include <QtGui/private/qtextengine_p.h>
#undef protected
#undef private
#include <QTextLayout>
#include <QTextCharFormat>
#include <QRawFont>
#include <QGlyphRun>
#include <QPainter>
#include <QImage>
#include <QCoreApplication>
#include <QVector>
#include <QString>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <new>
#include <dlfcn.h>
#include <QtCore/qmath.h>
#include <time.h>

namespace {
// WebKit's Qt 5.2 TextRun. Installation also verifies the instructions which
// access these fields; a different renderer ABI must leave the fix inactive.
struct Run {
    const void *data;
    unsigned available, length;
    float x, stretch, expansion;
    unsigned flags, tabs;
    void *context;
};
using BlockFn = Run *(*)(Run *, void *, void *, void *, unsigned, unsigned, void *, unsigned);
using InlineFn = Run *(*)(Run *, void *, void *, void *, void *, int, void *);
using FormatFn = void (*)(void *, QTextLayout *, const Run *);
using WidthFn = float (*)(void *, const Run *, void *, void *);
using CacheFn = float *(*)(void *, const Run *, float);
BlockFn original_block;
InlineFn original_inline;
FormatFn original_format;
WidthFn complex_width;
CacheFn original_width_cache;
ntf_italic_scope_fn reader_scope;
ntf_italic_logger_fn log_message;
bool active = false;
const int spacing_property = QTextFormat::UserProperty + 16;
struct Pending {
    Run *run;
    void *renderer;
    void *font;
    constexpr Pending(Run *r = nullptr, void *object = nullptr, void *face = nullptr)
        : run(r), renderer(object), font(face) { }
};
thread_local Pending pending = {};
thread_local bool measuring = false;

template <typename T> T field(const void *p, unsigned offset) {
    T value;
    std::memcpy(&value, static_cast<const char *>(p) + offset, sizeof value);
    return value;
}
void *pointer(const void *p, unsigned offset) { return field<void *>(p, offset); }
unsigned flags(const void *renderer) { return field<unsigned>(renderer, 24); }
bool is_text(const void *renderer) { return flags(renderer) & (1u << 8); }
bool is_inline(const void *renderer) { return flags(renderer) & (1u << 10); }
bool horizontal(const void *renderer) { return flags(renderer) & (1u << 12); }
void *style_font(void *renderer) {
    void *style = pointer(renderer, 4);
    return static_cast<char *>(pointer(style, 28)) + 16;
}
bool italic(const void *font) { return field<unsigned char>(font, 28) & 16; }
bool ordinary_font(const void *font) {
    return !(field<unsigned char>(font, 28) & 33) && field<float>(font, 48) == 0;
}
bool ordinary_style(const void *renderer, bool right_edge) {
    const void *style = pointer(renderer, 4);
    if (((field<unsigned>(style, 48) >> 16) & 15)
        || ((field<unsigned>(style, 40) >> 17) & 3) != 3) return false;
    const void *surround = pointer(style, 16);
    const unsigned side = right_edge ? 8 : 0;
    // StyleSurroundData: refcount, four offset lengths, four margin
    // lengths, then four padding lengths. LengthBox stores left first.
    return field<unsigned>(surround, 36 + side) == 0
        && field<unsigned>(surround, 68 + side) == 0;
}
struct Text {
    const void *data;
    unsigned length;
    bool eight;
    Text(const void *characters = nullptr, unsigned size = 0, bool is_eight = false)
        : data(characters), length(size), eight(is_eight) { }
    QChar at(unsigned i) const {
        return QChar(eight ? static_cast<const unsigned char *>(data)[i]
                          : static_cast<const unsigned short *>(data)[i]);
    }
    QString tail(unsigned count) const {
        QString result;
        for (unsigned i = length > count ? length - count : 0; i < length; ++i) result += at(i);
        return result;
    }
};
Text text(void *renderer) {
    void *impl = pointer(renderer, 48);
    if (!impl) return {};
    bool eight = field<unsigned>(impl, 16) & 64;
    // UTF-16 data is materialized by WebKit before constructing a 16-bit run.
    return {pointer(impl, 8), field<unsigned>(impl, 4), eight};
}
bool punctuation(QChar ch) {
    return QStringLiteral(".,;:!?\u2019\u201d'\")]").contains(ch);
}
void *next_text(void *left);
void remember(Run *run, void *renderer, void *font) {
    pending = {};
    if (!active || measuring || !renderer || !font || !run->length || !italic(font)
        || !ordinary_font(font) || !horizontal(renderer) || !(run->flags & 16)) return;
    Text t = text(renderer);
    if (!t.length || t.at(t.length - 1).isSpace()) return;
    uintptr_t end = reinterpret_cast<uintptr_t>(t.data) + t.length * (t.eight ? 1 : 2);
    uintptr_t run_end = reinterpret_cast<uintptr_t>(run->data) + run->length * ((run->flags & 4) ? 1 : 2);
    if (run_end != end && t.eight && !(run->flags & 4)) {
        using Data16Fn = const void *(*)(const void *);
        //nb lookup * 4.21.15015 * _ZNK3WTF10StringImpl17getData16SlowCaseEv
        static Data16Fn data16 = reinterpret_cast<Data16Fn>(dlsym(RTLD_DEFAULT, "_ZNK3WTF10StringImpl17getData16SlowCaseEv"));
        if (data16) end = reinterpret_cast<uintptr_t>(data16(pointer(renderer, 48))) + t.length * 2;
    }
    if (run_end != end) return;
    void *right = next_text(renderer);
    if (!right) return;
    const Text next = text(right);
    if (!next.length || !punctuation(next.at(0)) || italic(style_font(right))) return;
    pending = {run, renderer, font};
}
float *width_cache(void *cache, const Run *run, float entry) {
    // A width depends on the neighbouring style only for a candidate boundary.
    // WebKit's text-only cache cannot represent that distinction.
    if (active && pending.run == run && pending.renderer) return nullptr;
    return original_width_cache(cache, run, entry);
}
Run *block_run(Run *out, void *object, void *font, void *renderer,
               unsigned start, unsigned length, void *style, unsigned expansion) {
    Run *result = original_block(out, object, font, renderer, start, length, style, expansion);
    remember(out, renderer, font);
    return result;
}
Run *inline_run(Run *out, void *box, void *style, void *font,
                void *string, int available, void *hyphen) {
    Run *result = original_inline(out, box, style, font, string, available, hyphen);
    if (hyphen) pending = {};
    else remember(out, pointer(box, 16), font);
    return result;
}
// Only inline ancestors can connect a boundary. Never cross a block, replaced
// object, line break, or a pathological nesting depth.
void *next_text(void *left) {
    void *current = left;
    for (unsigned steps = 0; current && steps < 64; ++steps) {
        if (!ordinary_style(current, true)) return nullptr;
        void *next = pointer(current, 20);
        if (!next) {
            current = pointer(current, 12);
            if (!current || !is_inline(current)) return nullptr;
            continue;
        }
        current = next;
        for (unsigned depth = 0; current && depth < 32; ++depth) {
            if (!is_inline(current) || !horizontal(current)) return nullptr;
            if (!ordinary_style(current, false)) return nullptr;
            if (is_text(current)) {
                if (text(current).length) return current;
                break;
            }
            if (flags(current) & (1u << 9)) return nullptr;
            using ChildrenFn = void *(*)(void *);
            void *list = reinterpret_cast<ChildrenFn *>(pointer(current, 0))[9](current);
            if (!list) return nullptr;
            current = pointer(list, 0);
        }
    }
    return nullptr;
}
QRawFont raw_font(void *font, QChar initialize) {
    void *glyphs = pointer(font, 44);
    if (!glyphs) return {};
    void *primary = pointer(glyphs, 44);
    if (!primary || !pointer(primary, 52)) {
        unsigned short character = initialize.unicode();
        Run run = {&character, 1, 1, 0, 1, 0, 80, 0, nullptr};
        complex_width(font, &run, nullptr, nullptr);
        primary = pointer(glyphs, 44);
    }
    if (!primary) return {};
    void *platform = pointer(primary, 52);
    if (!platform) return {};
    return *reinterpret_cast<const QRawFont *>(static_cast<const char *>(platform) + 4);
}
// The final four painted glyphs, with their original shaping and advances.
// No second shaping pass is needed to decide which pixels need clearance.
struct InkRun {
    unsigned count = 0;
    quint32 glyphs[4] = {};
    QPointF positions[4];
    double advance = 0;
    bool operator==(const InkRun &other) const {
        if (count != other.count || advance != other.advance) return false;
        for (unsigned i = 0; i < count; ++i)
            if (glyphs[i] != other.glyphs[i] || positions[i] != other.positions[i]) return false;
        return true;
    }
};
struct Mask { QVector<int> left, right; };
Mask mask(QRawFont font, const InkRun &ink, double scale, double origin, double baseline,
          int width, int height) {
    font.setPixelSize(font.pixelSize() * scale);
    QVector<quint32> glyphs;
    QVector<QPointF> positions;
    for (unsigned i = 0; i < ink.count; ++i) {
        glyphs.append(ink.glyphs[i]); positions.append(ink.positions[i] * scale);
    }
    QGlyphRun run;
    run.setRawFont(font); run.setGlyphIndexes(glyphs); run.setPositions(positions);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) throw std::bad_alloc();
    image.fill(0);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    painter.drawGlyphRun(QPointF(origin, baseline), run);
    painter.end();
    Mask result;
    result.left.fill(100000, height); result.right.fill(-100000, height);
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) if (qAlpha(row[x]) >= 64) {
            result.left[y] = qMin(result.left[y], x); result.right[y] = x;
        }
    }
    return result;
}
int clearance(const Mask &first, const Mask &second, int radius) {
    int gap = 0, height = first.left.size();
    for (int y = 0; y < height; ++y) {
        int r = -100000, l = 100000;
        for (int row = qMax(0, y - radius); row <= qMin(height - 1, y + radius); ++row) {
            r = qMax(r, first.right[row]); l = qMin(l, second.left[row]);
        }
        if (r > -100000 && l < 100000) gap = qMax(gap, radius - (l - r - 1));
    }
    return gap;
}
struct Measurement {
    QRawFont left, right;
    InkRun ink;
    quint32 punctuation;
    double scale, amount;
};
QVector<Measurement> cache;
double correction(QRawFont a, QRawFont b, const InkRun &ink, QChar right, double scale) {
    const QVector<quint32> right_glyphs = b.glyphIndexesForString(QString(right));
    if (right_glyphs.size() != 1 || !right_glyphs[0] || !ink.count) return 0;
    for (const Measurement &entry : cache)
        if (entry.left == a && entry.right == b && entry.ink == ink
            && entry.punctuation == right_glyphs[0] && entry.scale == scale) return entry.amount;
    struct timespec started = {0, 0};
    if (log_message) clock_gettime(CLOCK_MONOTONIC, &started);
    const double size = a.pixelSize() * scale;
    if (!(size > 0 && size <= 1024)) return 0;
    int width = qCeil(ink.advance * scale + size * 5), height = qCeil(size * 4);
    if (width <= 0 || width > 4096 || height > 4096) return 0;
    InkRun punctuation;
    punctuation.count = 1; punctuation.glyphs[0] = right_glyphs[0];
    Mask first = mask(a, ink, scale, size * 2, size * 2, width, height);
    Mask second = mask(b, punctuation, scale, size * 2 + ink.advance * scale, size * 2, width, height);
    int gap = clearance(first, second, qMax(1, static_cast<int>(size / 40)));
    // Qt's complex path stores advances in 1/64-pixel fixed point. Round up
    // so that quantization cannot remove the last required painted pixel.
    double amount = gap > 0 && gap <= size / 2 ? std::ceil((gap + 1.0/64) * 64 / scale) / 64 : 0;
    if (cache.size() >= 128) cache.remove(0);
    cache.append({a, b, ink, right_glyphs[0], scale, amount});
    if (log_message) {
        struct timespec finished;
        clock_gettime(CLOCK_MONOTONIC, &finished);
        const double elapsed = (finished.tv_sec - started.tv_sec) * 1000.0
            + (finished.tv_nsec - started.tv_nsec) / 1000000.0;
        char message[256];
        std::snprintf(message, sizeof message, "Native italic: %s (%s) / %s (%s), size %.4f scale %.4f glyph U+%04x clearance %.4f in %.3f ms", a.familyName().toUtf8().constData(), a.styleName().toUtf8().constData(), b.familyName().toUtf8().constData(), b.styleName().toUtf8().constData(), a.pixelSize(), scale, right.unicode(), amount, elapsed);
        log_message(message);
    }
    return amount;
}

double boundary_amount(void *renderer, void *font, const InkRun &ink) {
    if (measuring || !renderer || !font || !ink.count) return 0;
    void *right = next_text(renderer);
    if (!right) return 0;
    Text next = text(right);
    if (!next.length || !punctuation(next.at(0))) return 0;
    void *other_font = style_font(right);
    if (italic(other_font) || !ordinary_font(other_font)) return 0;
    double scale = reader_scope(renderer);
    if (!(scale > 0 && scale <= 8)) return 0;
    measuring = true;
    double amount = 0;
    try {
        QRawFont a = raw_font(font, QChar('l')), b = raw_font(other_font, next.at(0));
        if (a.isValid() && b.isValid() && a.pixelSize() == b.pixelSize())
            amount = correction(a, b, ink, next.at(0), scale);
    } catch (...) { }
    measuring = false;
    return amount;
}
using AdvanceFn = unsigned (*)(void *, int, void *);
AdvanceFn original_advance;
unsigned advance(void *iterator, int to, void *buffer) {
    if (!active || measuring || !pending.run) return original_advance(iterator, to, buffer);
    Pending candidate = pending;
    const Run *run = static_cast<const Run *>(pointer(iterator, 4));
    void *font = pointer(iterator, 0);
    unsigned before = field<unsigned>(iterator, 8);
    unsigned result = original_advance(iterator, to, buffer);
    if (!run || candidate.run != run || candidate.font != font || !(run->flags & 16)
        || before >= run->length || field<unsigned>(iterator, 8) != run->length) return result;
    try {
    InkRun ink;
    if (buffer) {
        unsigned count = field<unsigned>(buffer, 8200);
        if (!count || count != field<unsigned>(buffer, 49184)) return result;
        const quint32 *glyphs = static_cast<const quint32 *>(pointer(buffer, 8204));
        const QPointF *advances = static_cast<const QPointF *>(pointer(buffer, 16408));
        void **fonts = static_cast<void **>(pointer(buffer, 0));
        void *platform = pointer(fonts[count - 1], 52);
        if (!platform || *reinterpret_cast<const QRawFont *>(static_cast<const char *>(platform) + 4) != raw_font(font, QChar('l')))
            return result;
        ink.count = qMin(4u, count);
        for (unsigned i = 0; i < ink.count; ++i) {
            unsigned index = count - ink.count + i;
            // Mixed fallback faces need a separate mask for each face.
            if (fonts[index] != fonts[count-1]) return result;
            if (!glyphs[index] || (glyphs[index] >> 24)) return result;
            ink.glyphs[i] = glyphs[index];
            ink.positions[i] = QPointF(ink.advance, 0);
            ink.advance += advances[index].x();
        }
    } else {
        // Without transforms WebKit uses one raw glyph and advance per character.
        QRawFont face = raw_font(font, QChar('l'));
        QString suffix = text(candidate.renderer).tail(4);
        QVector<quint32> glyphs = face.glyphIndexesForString(suffix);
        QVector<QPointF> advances = face.advancesForGlyphIndexes(glyphs);
        if (glyphs.size() > 4 || glyphs.size() != advances.size()) return result;
        ink.count = glyphs.size();
        for (unsigned i = 0; i < ink.count; ++i) {
            if (!glyphs[i]) return result;
            ink.glyphs[i] = glyphs[i]; ink.positions[i] = QPointF(ink.advance, 0);
            ink.advance += advances[i].x();
        }
    }
    double amount = boundary_amount(candidate.renderer, font, ink);
    if (amount > 0) {
        float width = field<float>(iterator, 12) + amount;
        std::memcpy(static_cast<char *>(iterator) + 12, &width, sizeof width);
        if (buffer) {
            const unsigned count = field<unsigned>(buffer, 49184);
            static_cast<QPointF *>(pointer(buffer, 16408))[count - 1].rx() += amount;
        }
    }
    } catch (...) { }
    return result;
}
void format(void *font, QTextLayout *layout, const Run *run) {
    Pending candidate = pending;
    pending = {};
    original_format(font, layout, run);
    if (measuring || candidate.run != run || candidate.font != font || !candidate.renderer) return;
    try {
    QList<QTextLayout::FormatRange> ranges = layout->additionalFormats();
    QTextLayout::FormatRange range;
    // Keep the run intact so the correction does not discard kerning. The
    // renderer lives through this synchronous layout; no pointer is retained.
    range.start = 0; range.length = layout->text().size();
    range.format.setProperty(spacing_property, QVariant::fromValue<qulonglong>(reinterpret_cast<uintptr_t>(candidate.renderer)));
    range.format.setProperty(spacing_property + 1, QVariant::fromValue<qulonglong>(reinterpret_cast<uintptr_t>(font)));
    ranges.append(range);
    layout->setAdditionalFormats(ranges);
    } catch (...) { }
}

}

void ntf_italic_native_reset() {
    pending = {};
    cache.clear();
}

bool ntf_italic_native_install(void *block, void *inl, void *fmt, void *width, void *cache_fn, void *advance_fn,
                               ntf_italic_scope_fn scope, ntf_italic_logger_fn logger) {
    if (!block || !inl || !fmt || !width || !cache_fn || !advance_fn || !scope || !ntf_shape_detour_active()) return false;
    complex_width = reinterpret_cast<WidthFn>(reinterpret_cast<uintptr_t>(width) | 1);
    reader_scope = scope; log_message = logger;
    // The context hooks are inert until the formatting hook has installed too.
    void *b = nullptr, *i = nullptr, *f = nullptr, *c = nullptr, *a = nullptr;
    if (ntf_detour_at(advance_fn, reinterpret_cast<void *>(&advance), &a, nullptr)) return false;
    original_advance = reinterpret_cast<AdvanceFn>(a);
    if (ntf_detour_at(cache_fn, reinterpret_cast<void *>(&width_cache), &c, nullptr)) return false;
    original_width_cache = reinterpret_cast<CacheFn>(c);
    if (ntf_detour_at(block, reinterpret_cast<void *>(&block_run), &b, nullptr)) return false;
    original_block = reinterpret_cast<BlockFn>(b);
    if (ntf_detour_at(inl, reinterpret_cast<void *>(&inline_run), &i, nullptr)) return false;
    original_inline = reinterpret_cast<InlineFn>(i);
    if (ntf_detour_at(fmt, reinterpret_cast<void *>(&format), &f, nullptr)) return false;
    original_format = reinterpret_cast<FormatFn>(f);
    ntf_shape_postprocess([](const void *engine, const void *item, int length, int count) {
        try {
            ntf_italic_native_adjust(static_cast<const QTextEngine *>(engine),
                                     *static_cast<const QScriptItem *>(item), length, count);
        } catch (...) { }
    });
    if (QCoreApplication *app = QCoreApplication::instance())
        QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] { ntf_italic_native_reset(); });
    active = true;
    return true;
}

void ntf_italic_native_adjust(const QTextEngine *engine, const QScriptItem &item,
                               int length, int count) {
    if (!active || count <= 0 || !engine->specialData || !engine->layoutData
        || (item.analysis.bidiLevel & 1)
        || item.position + length != engine->layoutData->string.size()) return;
    const QTextCharFormat format = engine->format(&item);
    void *renderer = reinterpret_cast<void *>(static_cast<uintptr_t>(format.property(spacing_property).toULongLong()));
    void *font = reinterpret_cast<void *>(static_cast<uintptr_t>(format.property(spacing_property + 1).toULongLong()));
    if (!renderer || !font) return;
    QGlyphLayout glyphs = engine->availableGlyphs(&item).mid(0, count);
    InkRun ink;
    ink.count = qMin(4, count);
    for (unsigned i = 0; i < ink.count; ++i) {
        unsigned index = count - ink.count + i;
        if (!glyphs.glyphs[index] || (glyphs.glyphs[index] >> 24)) return;
        ink.glyphs[i] = glyphs.glyphs[index];
        ink.positions[i] = QPointF(ink.advance + glyphs.offsets[index].x.toReal(), glyphs.offsets[index].y.toReal());
        ink.advance += glyphs.advances_x[index].toReal();
    }
    double amount = boundary_amount(renderer, font, ink);
    if (!(amount > 0 && amount <= 512)) return;
    glyphs.advances_x[count - 1] += QFixed::fromReal(amount);
}
