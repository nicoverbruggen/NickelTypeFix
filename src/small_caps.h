#ifndef NTF_SMALL_CAPS_H
#define NTF_SMALL_CAPS_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// FIX 14 — real small caps. Two seams, both required: a detour on QTextEngine::fontEngine, and a
// post-pass inside the fix 12 shaper detour. Either missing means the fix sits out.
typedef struct ntf_smallcaps_status_t {
    bool installed;         // the fontEngine detour is in place and the shaper detour is running
} ntf_smallcaps_status_t;

// A one-line logger the fix can call for rare events (a font classified, a detour refused).
// Lines go to the mod's verbose log; NULL means silent.
typedef void (*ntf_smallcaps_logger)(const char *line);

// Called once from ntf_init, after the shaper detour is known to be running. font_engine_sym is
// QTextEngine::fontEngine as NickelHook resolved it (may be NULL: the fix then sits out).
ntf_smallcaps_status_t ntf_smallcaps_enable(void *font_engine_sym, bool shaper_detour_running,
                                            ntf_smallcaps_logger logger);

#ifdef __cplusplus
}

// The shaping post-pass, for shape_cache.cc only. Both files compile against Qt's private headers,
// so the Qt types are declared here rather than hidden behind void pointers.
class QTextEngine;
struct QScriptItem;
class QFontEngine;
template <typename T> class QVector;

typedef int (*ntf_shape_fn)(const QTextEngine *, const QScriptItem &, const unsigned short *, int,
                            QFontEngine *, const QVector<unsigned int> &, bool);

// Returns the glyph count it produced, or -1 when the item is not a small caps run this fix
// handles, in which case the caller runs the real shaper as usual.
int ntf_smallcaps_shape(const QTextEngine *e, const QScriptItem &si, const unsigned short *string,
                        int itemLength, QFontEngine *fontEngine,
                        const QVector<unsigned int> &itemBoundaries, bool kerningEnabled,
                        ntf_shape_fn original);
#endif
#endif
