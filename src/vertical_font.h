#ifndef NTF_VERTICAL_FONT_H
#define NTF_VERTICAL_FONT_H

class QRawFont;

// Startup only. Kobo's vertical extension needs a legacy HB_Face even when Qt uses NG.
// All symbols are optional; a missing dependency must prevent the switch to NG.
bool ntf_vertical_font_prepare(void *new_face, void *load_face, void *free_face,
                               void *plugin_interface);
bool ntf_vertical_font_has_glyphs(const QRawFont &font);
unsigned ntf_vertical_font_substitute(const QRawFont &font, unsigned *glyphs, unsigned count);

#endif
