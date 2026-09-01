#include <cassert>
#include <cstddef>

#include "../../src/line_spacing_values.h"

int main() {
    static const double expected[] = {
        0.80, 0.81, 0.82, 0.83, 0.84, 0.86, 0.88, 0.90,
        0.92, 0.94, 0.96, 0.98, 1.00, 1.02, 1.05, 1.07,
        1.10, 1.15, 1.20, 1.25, 1.30, 1.35, 1.40, 1.50,
    };
    assert(ntf_line_spacing_24_value_count == 24);
    assert(ntf_line_spacing_24_value_count == sizeof(expected) / sizeof(expected[0]));
    // 0.86 must stay at index 5: Kobo reads that position as the default for a book with no
    // stored line spacing, so moving it would change what every new book opens at.
    assert(ntf_line_spacing_24_values[5] == 0.86);
    // Nothing below 0.80: tighter than that, consecutive lines' ink overlaps and no page
    // boundary is clean.
    assert(ntf_line_spacing_24_values[0] == 0.80);
    for (std::size_t i = 0; i < ntf_line_spacing_24_value_count; i++) {
        assert(ntf_line_spacing_24_values[i] == expected[i]);
        if (i) assert(ntf_line_spacing_24_values[i] > ntf_line_spacing_24_values[i - 1]);
    }
    return 0;
}
