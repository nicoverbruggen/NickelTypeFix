#ifndef NTF_ITALIC_SPACING_H
#define NTF_ITALIC_SPACING_H

typedef double (*ntf_italic_scope_fn)(const void *renderer);
typedef void (*ntf_italic_logger_fn)(const char *message);
void ntf_italic_native_reset();
bool ntf_italic_native_install(void *block_run, void *inline_run, void *format,
                               void *complex_width, void *width_cache, void *advance, ntf_italic_scope_fn scope,
                               ntf_italic_logger_fn logger);
class QTextEngine;
struct QScriptItem;
void ntf_italic_native_adjust(const QTextEngine *, const QScriptItem &, int length, int glyphs);

#endif
