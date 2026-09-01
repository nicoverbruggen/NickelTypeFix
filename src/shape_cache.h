#ifndef NTF_SHAPE_CACHE_H
#define NTF_SHAPE_CACHE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// FIX 12 — fast text shaping. Two independent steps, either of which can sit out.
typedef struct ntf_shape_status_t {
    bool ng_enabled;        // Qt now shapes with HarfBuzz NG instead of its 2007 shaper
    bool cache_installed;   // the shaped-result cache sits in front of whichever shaper runs
} ntf_shape_status_t;

// Called once from ntf_init, with the three QTextEngine symbols NickelHook resolved. Any of them
// may be NULL: the fix then changes nothing and reports what it could not do.
//   shape_text  QTextEngine::shapeText(int), read to locate Qt's shaper selector
//   shaper_old  QTextEngine::shapeTextWithHarfbuzz(...)
//   shaper_ng   QTextEngine::shapeTextWithHarfbuzzNG(...)
ntf_shape_status_t ntf_shape_cache_enable(void *shape_text, void *shaper_old, void *shaper_ng);

// Detour an exported function by rewriting its prologue, for probes that need to observe code
// libnickel does not reach through the PLT. Returns 0 on success and leaves the original callable
// through *original. Negative values report which step failed.
int ntf_detour_symbol(const char *symbol, void *replacement, void **original);

// Same, at a raw address, for code with no symbol at all. Relocates whole instructions and
// refuses anything PC-relative. relocated_out, if given, receives how many bytes moved.
int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out);

// Counters for the verbose log. Any pointer may be NULL.
void ntf_shape_cache_stats(unsigned long *calls, unsigned long *hits, unsigned long *records);

#ifdef __cplusplus
}
#endif
#endif
