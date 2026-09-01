#ifndef NTF_PAGECUT_GEOMETRY_H
#define NTF_PAGECUT_GEOMETRY_H

enum ntf_pagecut_refusal_reason {
    NTF_PAGECUT_NOT_A_LINE,
    NTF_PAGECUT_ADVANCE_TOO_SMALL,
    NTF_PAGECUT_NO_LINE_RHYTHM,
};

enum ntf_pagecut_rhythm_source {
    NTF_PAGECUT_NO_RHYTHM,
    NTF_PAGECUT_PREVIOUS_LINE,
    NTF_PAGECUT_FOLLOWING_LINE,
};

static inline bool ntf_pagecut_rhythm_height(long long a, long long b) {
    if (a <= 0 || b <= 0) return false;
    long long lo = a < b ? a : b;
    long long hi = a > b ? a : b;
    long long tolerance = lo / 16;
    if (tolerance < 2) tolerance = 2;
    return hi - lo <= tolerance;
}

static inline bool ntf_pagecut_rough_line_height(long long a, long long b) {
    if (a <= 0 || b <= 0) return false;
    long long lo = a < b ? a : b;
    long long hi = a > b ? a : b;
    return 2 * lo >= hi;
}

static inline bool ntf_pagecut_same_advance(long long a, long long b) {
    long long delta = a - b;
    return delta >= -1 && delta <= 1;
}

static inline bool ntf_pagecut_prior_rhythm_height(long long recorded_height,
                                                    long long line_height,
                                                    long long advance) {
    // The trim runs in place from top to bottom. A preceding line may already have height equal
    // to its advance instead of its original line-box height.
    return ntf_pagecut_rhythm_height(recorded_height, line_height)
        || ntf_pagecut_same_advance(recorded_height, advance);
}

template <typename Rect>
static ntf_pagecut_rhythm_source ntf_pagecut_line_rhythm(const Rect *rects, int n, int i, int j,
                                                         long long advance) {
    long long top = rects[i].y();
    long long next_top = rects[j].y();
    long long height = rects[i].height();
    long long next_height = rects[j].height();
    if (!ntf_pagecut_rhythm_height(height, next_height)) return NTF_PAGECUT_NO_RHYTHM;

    for (int k = i - 1; k >= 0; k--) {
        long long prior_advance = top - (long long)rects[k].y();
        if (prior_advance <= 0) continue;
        if (prior_advance > advance + 1) break;
        if (ntf_pagecut_same_advance(prior_advance, advance)
            && ntf_pagecut_prior_rhythm_height(rects[k].height(), height, advance))
            return NTF_PAGECUT_PREVIOUS_LINE;
    }
    for (int k = j + 1; k < n; k++) {
        long long following_advance = (long long)rects[k].y() - next_top;
        if (following_advance <= 0) continue;
        if (following_advance > advance + 1) break;
        if (ntf_pagecut_same_advance(following_advance, advance)
            && ntf_pagecut_rhythm_height(next_height, rects[k].height()))
            return NTF_PAGECUT_FOLLOWING_LINE;
    }
    return NTF_PAGECUT_NO_RHYTHM;
}

// Remove overlaps only from the private rectangle vector which Kobo uses to paginate. The real
// line boxes are retained separately for painting. A small overlap is safe to trim directly. A
// large overlap needs a repeated line rhythm, which distinguishes tightly spaced lines from a
// shifted inline run or a drop cap.
template <typename Rect, typename Observer>
static int ntf_pagecut_trim_geometry(Rect *rects, int n, int *guard_skips, Observer &observer) {
    if (guard_skips) *guard_skips = 0;
    if (!rects || n <= 1) return 0;

    int trimmed = 0;
    int refused = 0;
    for (int i = 0; i < n; i++) {
        long long top = rects[i].y();
        long long height = rects[i].height();
        int confirmed = -1;
        ntf_pagecut_rhythm_source confirmed_by = NTF_PAGECUT_NO_RHYTHM;
        int small_safe = -1;
        int first_overlap = -1;
        ntf_pagecut_refusal_reason first_reason = NTF_PAGECUT_NOT_A_LINE;

        for (int j = i + 1; j < n; j++) {
            long long next_top = rects[j].y();
            long long advance = next_top - top;
            if (advance <= 0) continue;
            if (advance >= height) break;

            long long next_height = rects[j].height();
            long long overlap = height - advance;
            if (first_overlap < 0) first_overlap = j;
            if (!ntf_pagecut_rough_line_height(height, next_height)) {
                if (first_overlap == j) first_reason = NTF_PAGECUT_NOT_A_LINE;
                continue;
            }
            if (overlap <= height / 4) {
                small_safe = j;
                break;
            }
            // Reject an advance so much smaller than the box that the pair cannot be two lines:
            // vertically shifted inline runs, where the "advance" is a few pixels. Measured over
            // real chapters, genuine lines sit at advance/height 0.47 at the tightest spacing the
            // reader offers (0.70) and 0.88 at 1.30, while shifted runs are around 0.14. A
            // half-height cutoff falls inside the legitimate range: at 0.70 it refused 2651 of
            // 2900 boxes and brought back the clipping this trim exists to prevent.
            if (3 * advance < height) {
                if (first_overlap == j) first_reason = NTF_PAGECUT_ADVANCE_TOO_SMALL;
                break;
            }
            ntf_pagecut_rhythm_source rhythm =
                ntf_pagecut_line_rhythm(rects, n, i, j, advance);
            if (rhythm != NTF_PAGECUT_NO_RHYTHM) {
                confirmed = j;
                confirmed_by = rhythm;
                break;
            }
            if (first_overlap == j) first_reason = NTF_PAGECUT_NO_LINE_RHYTHM;
            break;
        }

        int j = confirmed >= 0 ? confirmed : small_safe;
        if (j >= 0) {
            long long advance = (long long)rects[j].y() - rects[i].y();
            if (confirmed >= 0 && height - advance > height / 4)
                observer.accepted_large(i, top, height, advance,
                                        rects[j].y(), rects[j].height(), confirmed_by);
            rects[i].setHeight((int)advance);
            trimmed++;
            continue;
        }
        if (first_overlap >= 0) {
            int j0 = first_overlap;
            long long next_top = rects[j0].y();
            long long next_height = rects[j0].height();
            long long advance = next_top - top;
            refused++;
            observer.refused(i, top, height, advance, next_top, next_height, first_reason);
        }
    }
    if (guard_skips) *guard_skips = refused;
    return trimmed;
}

// Corrected page rectangles overlap: a page starts at its first owned line but keeps Kobo's stock
// bottom so its last owned line can finish. WebKit may therefore submit one line from either
// neighbour. Keep only origins in the page's half-open ownership interval. Invalid coordinates
// and invalid upper bounds fail open instead of hiding text.
static inline bool ntf_pagecut_owns_glyph(double origin_y, int page_top,
                                          bool corrected_start, int page_end,
                                          bool corrected_end) {
    bool after_start = !corrected_start || !(origin_y < page_top);
    bool valid_end = corrected_end && page_end > page_top;
    bool before_end = !valid_end || !(origin_y >= page_end);
    return after_start && before_end;
}

// pageRect can extend past the paint device after its top moves upward. WebKit clips such a page
// to the viewport before QWebFrame::render receives it. The render is complete when it covers the
// smaller of the corrected page and the viewport on each axis. An offset or smaller region is a
// partial repaint and cannot identify an unknown reader frame.
static inline bool ntf_pagecut_render_matches_page(int x, int y, int width, int height,
                                                   int page_width, int page_height,
                                                   int viewport_width, int viewport_height) {
    if (x != 0 || y != 0 || width <= 0 || height <= 0
        || page_width <= 0 || page_height <= 0
        || viewport_width <= 0 || viewport_height <= 0)
        return false;
    int expected_width = page_width < viewport_width ? page_width : viewport_width;
    int expected_height = page_height < viewport_height ? page_height : viewport_height;
    return width == expected_width && height == expected_height;
}

// Find the top of a line box which a stock page boundary cuts through. The boundary must also be
// the end of an earlier, similarly sized line box. Requiring both facts keeps unrelated nested
// rectangles, images, and drop caps from authorizing a move.
template <typename Rect>
static bool ntf_pagecut_snap_boundary(const Rect *rects, int n, int boundary, int *snapped) {
    if (!rects || n <= 1 || !snapped) return false;

    long long best_top = 0;
    long long best_height = 0;
    bool found = false;
    for (int i = 0; i < n; i++) {
        long long ending_top = rects[i].y();
        long long ending_height = rects[i].height();
        if (ending_height <= 0 || ending_top + ending_height != boundary) continue;

        for (int j = 0; j < n; j++) {
            long long top = rects[j].y();
            long long height = rects[j].height();
            long long end = top + height;
            if (top <= ending_top || top >= boundary || end <= boundary) continue;
            if (!ntf_pagecut_rough_line_height(ending_height, height)) continue;

            // A line can contain shorter inline boxes which also happen to end at the page
            // boundary. Prefer the matching pair with the larger common height so those boxes
            // cannot replace the enclosing line pair. For equal-sized pairs, the later top is
            // the line nearest the stock boundary.
            long long common_height = ending_height < height ? ending_height : height;
            if (!found || common_height > best_height
                || (common_height == best_height && top > best_top)) {
                found = true;
                best_height = common_height;
                best_top = top;
            }
        }
    }
    if (!found || best_top < -2147483647LL - 1 || best_top > 2147483647LL)
        return false;

    *snapped = (int)best_top;
    return true;
}

// A corrected start can make a later page taller than the paint viewport. Find the first owned
// line box which cannot fit and make it the following page's start. A candidate needs another
// similarly sized box at a plausible line advance. This rejects isolated images, drop caps, and
// vertically shifted runs. Short inline boxes cannot win merely because they share a line top.
template <typename Rect>
static bool ntf_pagecut_fit_boundary(const Rect *rects, int n, int page_top, int page_end,
                                     int viewport_height, int *fitted) {
    if (!rects || n <= 1 || !fitted || viewport_height <= 0 || page_end <= page_top)
        return false;

    long long limit = (long long)page_top + viewport_height;
    long long best_top = 0;
    bool found = false;
    for (int i = 0; i < n; i++) {
        long long top = rects[i].y();
        long long height = rects[i].height();
        if (height <= 0 || 2 * height > viewport_height
            || top <= page_top || top >= page_end || top + height <= limit)
            continue;

        bool has_peer = false;
        for (int j = 0; j < n; j++) {
            long long peer_top = rects[j].y();
            long long peer_height = rects[j].height();
            if (peer_height <= 0 || peer_top < page_top || peer_top >= page_end
                || peer_top == top
                || !ntf_pagecut_rough_line_height(height, peer_height))
                continue;
            long long advance = top > peer_top ? top - peer_top : peer_top - top;
            long long smaller_height = height < peer_height ? height : peer_height;
            long long larger_height = height > peer_height ? height : peer_height;
            if (2 * advance < smaller_height || advance > 2 * larger_height) continue;
            has_peer = true;
            break;
        }
        if (!has_peer) continue;
        if (!found || top < best_top) {
            found = true;
            best_top = top;
        }
    }
    if (!found || best_top < -2147483647LL - 1 || best_top > 2147483647LL)
        return false;

    *fitted = (int)best_top;
    return true;
}

#endif
