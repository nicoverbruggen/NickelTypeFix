#include <climits>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/pagecut_geometry.h"

struct TestRect {
    int top;
    int h;

    int y() const { return top; }
    int height() const { return h; }
    void setHeight(int value) { h = value; }
};

struct TestObserver {
    int reasons[3];
    int accepted[3];

    TestObserver() : reasons(), accepted() { }

    void refused(int, long long, long long, long long, long long, long long,
                 ntf_pagecut_refusal_reason reason) {
        reasons[reason]++;
    }

    void accepted_large(int, long long, long long, long long, long long, long long,
                        ntf_pagecut_rhythm_source source) {
        accepted[source]++;
    }
};

static void fail(const char *name, const char *message) {
    std::fprintf(stderr, "%s: %s\n", name, message);
    std::exit(1);
}

static void require(bool condition, const char *name, const char *message) {
    if (!condition) fail(name, message);
}

static void test_pagination_trim_at_all_optional_values() {
    const int values[] = {
        70, 73, 76, 80, 84, 86, 88, 90, 92, 94, 96, 98,
        100, 102, 105, 107, 110, 115, 120, 125, 130, 135, 140, 150,
    };
    const int height = 70;
    for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
        std::vector<TestRect> rects;
        for (int line = 0; line < 5; line++) {
            int top = (line * height * values[v] + 50) / 100;
            rects.push_back(TestRect{top, height});
        }
        TestObserver observer;
        int refused = 0;
        int trimmed = ntf_pagecut_trim_geometry(
            rects.data(), (int)rects.size(), &refused, observer);
        int expected = values[v] < 100 ? 4 : 0;
        if (trimmed != expected)
            fail("all optional spacing values", "unexpected number of trimmed lines");
        require(refused == 0,
                "all optional spacing values", "a regular line sequence was refused");
    }
}

static void test_exact_device_pagination_trim() {
    TestRect rects[] = {
        {3340, 70}, {3388, 70}, {3436, 70}, {3436, 70}, {3484, 70}, {3532, 70},
    };
    TestObserver observer;
    int refused = 0;
    int trimmed = ntf_pagecut_trim_geometry(rects, 6, &refused, observer);
    require(trimmed == 5,
            "device pagination trim", "the 48 px line rhythm was not normalized");
    require(refused == 0,
            "device pagination trim", "the measured line rhythm was refused");
    for (int i = 0; i < 5; i++)
        require(rects[i].height() == 48,
                "device pagination trim", "a box did not end at the following line start");
    require(rects[5].height() == 70,
            "device pagination trim", "the last line was shortened without a following line");
}

static void test_pagination_trim_guards() {
    TestRect isolated[] = {{0, 70}, {48, 70}};
    TestObserver isolated_observer;
    int refused = 0;
    require(ntf_pagecut_trim_geometry(isolated, 2, &refused, isolated_observer) == 0,
            "pagination trim guards", "an unconfirmed large overlap was trimmed");
    require(refused == 1,
            "pagination trim guards", "the unconfirmed overlap was not reported");

    TestRect shifted[] = {{0, 70}, {10, 70}, {20, 70}};
    TestObserver shifted_observer;
    refused = 0;
    require(ntf_pagecut_trim_geometry(shifted, 3, &refused, shifted_observer) == 0,
            "pagination trim guards", "vertically shifted runs were treated as lines");

    TestRect drop_cap[] = {{0, 76}, {58, 209}};
    TestObserver drop_cap_observer;
    refused = 0;
    require(ntf_pagecut_trim_geometry(drop_cap, 2, &refused, drop_cap_observer) == 0,
            "pagination trim guards", "a drop cap was treated as the next line");
}

static void test_device_boundary() {
    const TestRect rects[] = {
        {1084, 74}, {1134, 74}, {1184, 74}, {1234, 74},
    };
    int snapped = 0;
    require(ntf_pagecut_snap_boundary(rects, 4, 1208, &snapped),
            "device boundary", "the measured cut was not recognized");
    require(snapped == 1184,
            "device boundary", "the measured cut did not move to the sliced line's top");
}

static void test_device_glyph_ownership() {
    require(ntf_pagecut_owns_glyph(1135.0, 0, false, 1184, true),
            "device glyph ownership", "page one lost its last owned line");
    require(!ntf_pagecut_owns_glyph(1185.0, 0, false, 1184, true),
            "device glyph ownership", "page two's first line leaked onto page one");
    require(!ntf_pagecut_owns_glyph(1135.0, 1184, true, 2384, true),
            "device glyph ownership", "the preceding line leaked onto page two");
    require(ntf_pagecut_owns_glyph(1185.0, 1184, true, 2384, true),
            "device glyph ownership", "page two lost its first owned line");
    require(ntf_pagecut_owns_glyph(2335.0, 1184, true, 2384, true),
            "device glyph ownership", "page two lost its last owned line");
    require(!ntf_pagecut_owns_glyph(2385.0, 1184, true, 2384, true),
            "device glyph ownership", "page three's first line leaked onto page two");
    require(!ntf_pagecut_owns_glyph(2335.0, 2384, true, 0, false),
            "device glyph ownership", "the preceding line leaked onto page three");
    require(ntf_pagecut_owns_glyph(2385.0, 2384, true, 0, false),
            "device glyph ownership", "page three lost its first owned line");
}

static void test_glyph_ownership_boundaries() {
    require(ntf_pagecut_owns_glyph(1184.0, 1184, true, 2384, true),
            "glyph ownership boundary", "a run at the page start was rejected");
    require(!ntf_pagecut_owns_glyph(2384.0, 1184, true, 2384, true),
            "glyph ownership boundary", "a run at the next page start was accepted");
    require(ntf_pagecut_owns_glyph(1135.0, 1184, false, 2384, false),
            "glyph ownership boundary", "an unchanged page filtered a glyph run");
    require(ntf_pagecut_owns_glyph(3000.0, 1184, true, 1184, true),
            "glyph ownership boundary", "an empty ownership interval hid text");
    require(ntf_pagecut_owns_glyph(3000.0, 1184, true, 1000, true),
            "glyph ownership boundary", "a reversed ownership interval hid text");
    volatile double zero = 0.0;
    double invalid = zero / zero;
    require(ntf_pagecut_owns_glyph(invalid, 1184, true, 2384, true),
            "glyph ownership boundary", "an invalid coordinate hid text");
}

static void test_device_render_geometry() {
    require(ntf_pagecut_render_matches_page(0, 0, 1022, 1184,
                                             1022, 1184, 1022, 1246),
            "device render geometry", "page one did not match its measured render");
    require(ntf_pagecut_render_matches_page(0, 0, 1022, 1200,
                                             1022, 1200, 1022, 1246),
            "device render geometry", "page two did not match its measured render");
    require(ntf_pagecut_render_matches_page(0, 0, 1022, 120,
                                             1022, 120, 1022, 1246),
            "device render geometry", "page three did not match its measured render");
    require(ntf_pagecut_render_matches_page(0, 0, 1022, 1246,
                                             1022, 1257, 1022, 1246),
            "device render geometry", "a complete viewport-clipped page was rejected");
}

static void test_render_geometry_guards() {
    require(!ntf_pagecut_render_matches_page(1, 0, 1022, 1184,
                                              1022, 1184, 1022, 1246),
            "render geometry guards", "an offset render was accepted");
    require(!ntf_pagecut_render_matches_page(0, 1, 1022, 1184,
                                              1022, 1184, 1022, 1246),
            "render geometry guards", "an offset render was accepted");
    require(!ntf_pagecut_render_matches_page(0, 0, 1021, 1184,
                                              1022, 1184, 1022, 1246),
            "render geometry guards", "a width mismatch was accepted");
    require(!ntf_pagecut_render_matches_page(0, 0, 1022, 1183,
                                              1022, 1184, 1022, 1246),
            "render geometry guards", "a height mismatch was accepted");
    require(!ntf_pagecut_render_matches_page(0, 629, 1022, 617,
                                              1022, 1257, 1022, 1246),
            "render geometry guards", "the measured partial repaint was accepted");
    require(!ntf_pagecut_render_matches_page(0, 0, 0, 1184,
                                              0, 1184, 0, 1246),
            "render geometry guards", "an empty render was accepted");
    require(!ntf_pagecut_render_matches_page(0, 0, 1022, -1,
                                              1022, -1, 1022, 1246),
            "render geometry guards", "an invalid render was accepted");
    require(!ntf_pagecut_render_matches_page(0, 0, 1022, 1184,
                                              1022, 1184, 1022, 0),
            "render geometry guards", "an invalid viewport was accepted");
}

static void test_small_stock_overlap() {
    const TestRect rects[] = {{0, 63}, {61, 63}, {122, 63}};
    int snapped = 0;
    require(ntf_pagecut_snap_boundary(rects, 3, 63, &snapped),
            "small overlap", "the stock two-pixel cut was not recognized");
    require(snapped == 61, "small overlap", "the boundary did not move to the next line");
}

static void test_clean_boundary() {
    const TestRect rects[] = {{0, 63}, {63, 63}};
    int snapped = 0;
    require(!ntf_pagecut_snap_boundary(rects, 2, 63, &snapped),
            "clean boundary", "a boundary between touching boxes moved");
}

static void test_boundary_without_ending_line() {
    const TestRect rects[] = {{0, 74}, {50, 74}};
    int snapped = 0;
    require(!ntf_pagecut_snap_boundary(rects, 2, 60, &snapped),
            "no ending line", "an arbitrary point inside a box moved");
}

static void test_drop_cap() {
    const TestRect rects[] = {{0, 76}, {58, 209}};
    int snapped = 0;
    require(!ntf_pagecut_snap_boundary(rects, 2, 76, &snapped),
            "drop cap", "a drop cap was accepted as the next line");
}

static void test_same_top_runs() {
    const TestRect rects[] = {{0, 63}, {0, 60}, {50, 63}, {50, 60}};
    int snapped = 0;
    require(ntf_pagecut_snap_boundary(rects, 4, 63, &snapped),
            "same-top runs", "repeated runs hid the next line");
    require(snapped == 50, "same-top runs", "the wrong repeated run was selected");
}

static void test_nested_short_run() {
    const TestRect rects[] = {{0, 74}, {50, 20}, {50, 74}};
    int snapped = 0;
    require(ntf_pagecut_snap_boundary(rects, 3, 74, &snapped),
            "nested short run", "the full next line was not found");
    require(snapped == 50, "nested short run", "the short inline run changed the snap");
}

static void test_only_nested_short_run() {
    const TestRect rects[] = {{0, 74}, {50, 20}};
    int snapped = 0;
    require(!ntf_pagecut_snap_boundary(rects, 2, 74, &snapped),
            "only nested short run", "an inline run authorized a boundary move");
}

static void test_inline_runs_do_not_override_full_lines() {
    const TestRect rects[] = {
        {1134, 74}, {1184, 74},
        {1188, 20}, {1194, 20},
    };
    int snapped = 0;
    require(ntf_pagecut_snap_boundary(rects, 4, 1208, &snapped),
            "inline override", "the full overlapping lines were not recognized");
    require(snapped == 1184,
            "inline override", "short inline runs overrode the full line boundary");
}

static void test_device_pages_move_the_first_line_which_cannot_fit() {
    const TestRect rects[] = {
        {1089, 68}, {1135, 68}, {1181, 68}, {1227, 68},
        {2285, 68}, {2331, 68}, {2377, 68}, {2377, 20}, {2423, 68},
        {3476, 67}, {3522, 67}, {3568, 67}, {3568, 20}, {3614, 67},
        {3716, 68},
    };
    const int n = sizeof(rects) / sizeof(rects[0]);
    int boundary = 0;

    require(!ntf_pagecut_fit_boundary(rects, n, 0, 1181, 1246, &boundary),
            "device page fit", "a page whose owned lines fit was shortened");
    require(ntf_pagecut_fit_boundary(rects, n, 1181, 2423, 1246, &boundary),
            "device page fit", "the measured Libron overflow was not recognized");
    require(boundary == 2377,
            "device page fit", "the split Libron line was not moved to the next page");
    require(ntf_pagecut_fit_boundary(rects, n, boundary, 3716, 1246, &boundary),
            "device page fit", "the correction did not propagate to the following page");
    require(boundary == 3568,
            "device page fit", "the following page still owned a line which could not fit");
}

static void test_page_fit_guards() {
    int boundary = 99;
    const TestRect no_peer[] = {{0, 68}, {1181, 68}};
    require(!ntf_pagecut_fit_boundary(no_peer, 2, 0, 1240, 1200, &boundary),
            "page fit guards", "an isolated box was accepted as a line");
    require(boundary == 99,
            "page fit guards", "a rejected fit changed the output boundary");

    const TestRect shifted_runs[] = {{1100, 68}, {1110, 68}, {1120, 68}};
    require(!ntf_pagecut_fit_boundary(shifted_runs, 3, 100, 1200, 1020, &boundary),
            "page fit guards", "vertically shifted runs were accepted as separate lines");

    const TestRect oversized[] = {{100, 700}, {500, 700}};
    require(!ntf_pagecut_fit_boundary(oversized, 2, 0, 1000, 900, &boundary),
            "page fit guards", "an object taller than half a page was accepted as a line");

    const TestRect valid[] = {{100, 68}, {146, 68}};
    require(!ntf_pagecut_fit_boundary(valid, 2, 0, 200, 0, &boundary),
            "page fit guards", "an invalid viewport produced a boundary");
    require(!ntf_pagecut_fit_boundary(valid, 2, 200, 100, 1000, &boundary),
            "page fit guards", "a reversed page interval produced a boundary");
}

static void test_extreme_coordinates() {
    const TestRect rects[] = {{INT_MIN, 1}, {INT_MAX - 1, 1}};
    int snapped = 0;
    require(!ntf_pagecut_snap_boundary(rects, 2, INT_MAX, &snapped),
            "extreme coordinates", "distant rectangles authorized a boundary move");
}


// A line box is taller than the ink it holds: the extra sits below the baseline as slack. Judging
// "does this line fit" on the raw box therefore moves lines whose ink would have rendered inside
// the page. Fix 9 already trims boxes to their real advance for pagination, and the fit test must
// use those same trimmed heights. This is the case that lost one line per page at ordinary line
// spacing, where nothing needed correcting at all.
static void test_fit_uses_the_advance_not_the_box() {
    // Seven lines on a 60px rhythm, each box 66 tall: every box overruns its own advance by the
    // 6px of slack that sits below the baseline. A 300px page holds exactly five of them.
    TestRect rects[7];
    for (int i = 0; i < 7; i++) { rects[i].top = i * 60; rects[i].h = 66; }

    const int page_top = 0;
    const int page_end = 420;
    const int viewport = 300;          // exactly five advances

    // Untrimmed, the fifth line (top 240, box ending 306) is judged not to fit on its slack alone,
    // and the boundary moves up to 240 -- one line early. That is the defect.
    int fitted_raw = 0;
    require(ntf_pagecut_fit_boundary(rects, 7, page_top, page_end, viewport, &fitted_raw),
            "fit uses the advance", "raw boxes reported no overflowing line");
    require(fitted_raw == 240,
            "fit uses the advance", "raw boxes did not move the boundary one line early");

    // Trimmed to their real advance, the fifth line fits and the boundary lands on the sixth,
    // which genuinely starts at the page bottom.
    int guard_skips = 0;
    TestObserver observer;
    ntf_pagecut_trim_geometry(rects, 7, &guard_skips, observer);
    require(rects[0].h == 60, "fit uses the advance", "trim did not reduce a box to its advance");

    int fitted_trimmed = 0;
    require(ntf_pagecut_fit_boundary(rects, 7, page_top, page_end, viewport, &fitted_trimmed),
            "fit uses the advance", "trimmed boxes reported no overflowing line");
    require(fitted_trimmed == 300,
            "fit uses the advance", "a line whose ink fits was still moved to the next page");
}

// The tightest line spacing the reader offers is 0.70, and there the advance falls to just under
// half the line box. A half-height floor therefore sits inside the range real lines occupy and
// refuses almost all of them, which puts the clipping back. Measured over a real chapter, the
// ratio of advance to box is 0.47 at its lowest across every offered spacing, while the shifted
// inline runs the floor exists to reject are around 0.14. These pin both ends of that gap.
static void test_trim_admits_the_tightest_line_spacing() {
    // 46px text at line spacing 0.70: a 66px box on a 31px advance, ratio 0.47. Three lines give
    // the middle one a rhythm on both sides.
    TestRect tightest[] = {{0, 66}, {31, 66}, {62, 66}};
    TestObserver observer;
    int refused = 0;
    require(ntf_pagecut_trim_geometry(tightest, 3, &refused, observer) > 0,
            "trim admits tightest spacing", "line spacing 0.70 was refused as too tight");
    require(tightest[0].h == 31,
            "trim admits tightest spacing", "a 0.70 line kept the overlap that clips it");
    require(observer.reasons[NTF_PAGECUT_ADVANCE_TOO_SMALL] == 0,
            "trim admits tightest spacing", "real lines were rejected for a small advance");

    // Just below the floor, and with a rhythm of its own, so only the floor can reject it. The
    // guard has to hold here or shifted runs come back.
    TestRect too_tight[] = {{0, 70}, {20, 70}, {40, 70}};
    TestObserver tight_observer;
    refused = 0;
    require(ntf_pagecut_trim_geometry(too_tight, 3, &refused, tight_observer) == 0,
            "trim admits tightest spacing", "an advance under a third of the box was trimmed");
    require(tight_observer.reasons[NTF_PAGECUT_ADVANCE_TOO_SMALL] == 2,
            "trim admits tightest spacing", "the small-advance guard did not report the refusal");
}

int main() {
    test_pagination_trim_at_all_optional_values();
    test_exact_device_pagination_trim();
    test_pagination_trim_guards();
    test_device_boundary();
    test_device_glyph_ownership();
    test_glyph_ownership_boundaries();
    test_device_render_geometry();
    test_render_geometry_guards();
    test_small_stock_overlap();
    test_clean_boundary();
    test_boundary_without_ending_line();
    test_drop_cap();
    test_same_top_runs();
    test_nested_short_run();
    test_only_nested_short_run();
    test_inline_runs_do_not_override_full_lines();
    test_device_pages_move_the_first_line_which_cannot_fit();
    test_page_fit_guards();
    test_extreme_coordinates();
    test_fit_uses_the_advance_not_the_box();
    test_trim_admits_the_tightest_line_spacing();
    std::puts("pagecut geometry tests passed");
    return 0;
}
