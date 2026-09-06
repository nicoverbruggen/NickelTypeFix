#ifndef NTF_FONT_DROPDOWN_H
#define NTF_FONT_DROPDOWN_H

class QString;
class QLabel;

// Calls the original setter once, then retains a changed, visible dropdown preview.
// Returns 1 for a held preview, 0 for passthrough, or -1 if capture failed.
int ntf_set_font_dropdown_text(QLabel *label, const QString &text,
                              void (*original)(QLabel *, const QString &));

// Called after Nickel finishes applying a font family. Returns the number of selected
// font labels rebuilt; a changed firmware layout or a call off the GUI thread is skipped.
int ntf_refresh_font_dropdown(const QString &family);

#endif
