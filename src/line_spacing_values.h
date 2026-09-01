#ifndef NTF_LINE_SPACING_VALUES_H
#define NTF_LINE_SPACING_VALUES_H

#include <cstddef>

// The replacement line-spacing choices. Keep this as plain data so the runtime
// hook and the host-side test use the same list.
//
// Index 5 is the value Kobo falls back to when a book has no stored line spacing
// (ContentSettings' constructor reads it through getLastReadingLineHeightScalar).
// Changing what sits at that position changes the reader's default, so 0.86 stays there.
//
// The list stops at 0.80. Below that the lines are packed closer than the text is tall: the ink
// of one line reaches into the next, so no page boundary can be drawn without cutting a glyph,
// whatever the page-boundary fix does. Measured on a real chapter through the device's own font
// engine, 0.80 is clean at every size and every page position, and 0.70 is not.
static const double ntf_line_spacing_24_values[] = {
    0.80, 0.81, 0.82, 0.83, 0.84, 0.86, 0.88, 0.90,
    0.92, 0.94, 0.96, 0.98, 1.00, 1.02, 1.05, 1.07,
    1.10, 1.15, 1.20, 1.25, 1.30, 1.35, 1.40, 1.50,
};

static const std::size_t ntf_line_spacing_24_value_count =
    sizeof(ntf_line_spacing_24_values) / sizeof(ntf_line_spacing_24_values[0]);

#endif
