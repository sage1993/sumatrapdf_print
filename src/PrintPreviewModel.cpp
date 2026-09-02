#include "base/Base.h"
#if IS_DEBUG
#include "base/UtAssert.h"
#endif

#include "SumatraDialogs.h"
#include "PrintLayout.h"
#include "PrintPreviewModel.h"

static bool IsRangeSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void SkipRangeSpace(Str text, int& pos) {
    while (pos < len(text) && IsRangeSpace(text.s[pos])) {
        pos++;
    }
}

static bool ParsePageNo(Str text, int pageCount, int& pos, int& pageNo) {
    SkipRangeSpace(text, pos);
    if (pos >= len(text) || text.s[pos] < '0' || text.s[pos] > '9') {
        return false;
    }

    int value = 0;
    while (pos < len(text) && text.s[pos] >= '0' && text.s[pos] <= '9') {
        int digit = text.s[pos] - '0';
        if (value > (pageCount - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        pos++;
    }
    if (value < 1 || value > pageCount) {
        return false;
    }

    pageNo = value;
    return true;
}

bool ParsePrintRanges(Str text, int pageCount, Vec<PRINTPAGERANGE>& ranges) {
    VecClear(ranges);
    if (pageCount < 1) {
        return false;
    }

    int pos = 0;
    SkipRangeSpace(text, pos);
    if (pos >= len(text)) {
        return false;
    }

    for (;;) {
        int first = 0;
        if (!ParsePageNo(text, pageCount, pos, first)) {
            VecClear(ranges);
            return false;
        }

        SkipRangeSpace(text, pos);
        int last = first;
        if (pos < len(text) && text.s[pos] == '-') {
            pos++;
            if (!ParsePageNo(text, pageCount, pos, last) || last < first) {
                VecClear(ranges);
                return false;
            }
            SkipRangeSpace(text, pos);
        }

        PRINTPAGERANGE range{};
        range.nFromPage = (DWORD)first;
        range.nToPage = (DWORD)last;
        VecAppend(ranges, range);

        if (pos == len(text)) {
            return true;
        }
        if (text.s[pos] != ',') {
            VecClear(ranges);
            return false;
        }
        pos++;
        SkipRangeSpace(text, pos);
        if (pos == len(text)) {
            VecClear(ranges);
            return false;
        }
    }
}

static bool IsValidDpi(float dpi) {
    return dpi > 0.f && isfinite(dpi);
}

static float PxToMm(float px, float dpi) {
    if (!IsValidDpi(dpi)) {
        return 0.f;
    }
    return px * 25.4f / dpi;
}

bool BuildPrinterMetrics(Size paperPx, Rect printablePx, float dpiX, float dpiY, WORD paperId, bool portrait,
                         bool duplexSupported, PrinterMetrics& out) {
    out = {};
    if (paperPx.dx <= 0 || paperPx.dy <= 0 || printablePx.dx <= 0 || printablePx.dy <= 0 || !IsValidDpi(dpiX) ||
        !IsValidDpi(dpiY)) {
        return false;
    }

    out.paperPx = paperPx;
    out.printablePx = printablePx;
    out.dpiX = dpiX;
    out.dpiY = dpiY;
    out.paperMm = {PxToMm((float)paperPx.dx, dpiX), PxToMm((float)paperPx.dy, dpiY)};
    out.printableMm = {PxToMm((float)printablePx.x, dpiX), PxToMm((float)printablePx.y, dpiY),
                       PxToMm((float)printablePx.dx, dpiX), PxToMm((float)printablePx.dy, dpiY)};
    out.paperId = paperId;
    out.portrait = portrait;
    out.duplexSupported = duplexSupported;
    return true;
}

struct ClipEdges {
    float left = 0.f;
    float right = 0.f;
    float top = 0.f;
    float bottom = 0.f;
};

static float Positive(float value) {
    return value > 0.f ? value : 0.f;
}

static ClipEdges OutsidePrintable(RectF target, Rect printable) {
    float printableRight = (float)(printable.x + printable.dx);
    float printableBottom = (float)(printable.y + printable.dy);
    float targetRight = target.x + target.dx;
    float targetBottom = target.y + target.dy;

    ClipEdges edges;
    edges.left = Positive((float)printable.x - target.x);
    edges.right = Positive(targetRight - printableRight);
    edges.top = Positive((float)printable.y - target.y);
    edges.bottom = Positive(targetBottom - printableBottom);
    return edges;
}

static bool HasClip(const ClipEdges& edges) {
    return edges.left > 0.f || edges.right > 0.f || edges.top > 0.f || edges.bottom > 0.f;
}

// Prefer actual-content overhang; fall back to page-box overhang for harmless clipping.
static float ReportedClip(float pageClip, float contentClip) {
    return contentClip > 0.f ? contentClip : pageClip;
}

ClippingReport CalculatePrintClipping(RectF pageTarget, RectF contentTarget, Rect printable, float dpiX, float dpiY) {
    ClipEdges page = OutsidePrintable(pageTarget, printable);
    ClipEdges content = OutsidePrintable(contentTarget, printable);

    ClippingReport report;
    report.pageBoundsOutsidePrintable = HasClip(page);
    report.contentOutsidePrintable = HasClip(content);
    report.leftMm = PxToMm(ReportedClip(page.left, content.left), dpiX);
    report.rightMm = PxToMm(ReportedClip(page.right, content.right), dpiX);
    report.topMm = PxToMm(ReportedClip(page.top, content.top), dpiY);
    report.bottomMm = PxToMm(ReportedClip(page.bottom, content.bottom), dpiY);
    return report;
}

void InvalidatePreview(PrintDialogState& state, PreviewInvalidation level) {
    if (level == PreviewInvalidation::None) {
        return;
    }

    state.previewGeneration++;
    if (state.previewGeneration == 0) {
        state.previewGeneration = 1;
    }
}

#if IS_DEBUG

static bool NearPreviewValue(float a, float b, float tolerance = 0.1f) {
    return fabsf(a - b) <= tolerance;
}

void PrintPreviewModel_UnitTests() {
    Vec<PRINTPAGERANGE> ranges;
    utassert(ParsePrintRanges(StrL("1"), 10, ranges));
    utassert(len(ranges) == 1 && ranges[0].nFromPage == 1 && ranges[0].nToPage == 1);

    utassert(ParsePrintRanges(StrL("1-3"), 10, ranges));
    utassert(len(ranges) == 1 && ranges[0].nFromPage == 1 && ranges[0].nToPage == 3);

    utassert(ParsePrintRanges(StrL("1-3,5"), 10, ranges));
    utassert(len(ranges) == 2 && ranges[1].nFromPage == 5 && ranges[1].nToPage == 5);

    utassert(ParsePrintRanges(StrL(" 1 - 3 , 5 "), 10, ranges));
    utassert(len(ranges) == 2);

    utassert(!ParsePrintRanges(StrL("0"), 10, ranges));
    utassert(!ParsePrintRanges(StrL("11"), 10, ranges));
    utassert(!ParsePrintRanges(StrL("3-1"), 10, ranges));
    utassert(!ParsePrintRanges(StrL("1,,2"), 10, ranges));
    utassert(!ParsePrintRanges(StrL("1,"), 10, ranges));
    utassert(!ParsePrintRanges(StrL(""), 10, ranges));
    utassert(len(ranges) == 0);

    PrinterMetrics metrics;
    utassert(BuildPrinterMetrics({2480, 3508}, {100, 100, 2280, 3308}, 300.f, 300.f, 9, true, true, metrics));
    utassert(NearPreviewValue(metrics.paperMm.dx, 209.97f));
    utassert(NearPreviewValue(metrics.paperMm.dy, 297.01f));
    utassert(metrics.paperId == 9 && metrics.portrait && metrics.duplexSupported);

    utassert(BuildPrinterMetrics({4961, 14031}, {120, 240, 4721, 13551}, 600.f, 1200.f, 8, true, false, metrics));
    utassert(NearPreviewValue(metrics.paperMm.dx, 210.015f));
    utassert(NearPreviewValue(metrics.paperMm.dy, 296.99f));
    utassert(NearPreviewValue(metrics.printableMm.x, 5.08f));
    utassert(NearPreviewValue(metrics.printableMm.y, 5.08f));
    utassert(!BuildPrinterMetrics({0, 14031}, {0, 0, 1, 1}, 600.f, 1200.f, 0, true, false, metrics));
    utassert(!BuildPrinterMetrics({100, 100}, {0, 0, 100, 100}, 0.f, 1200.f, 0, true, false, metrics));

    ClippingReport clipping = CalculatePrintClipping({90.f, 80.f, 1030.f, 1040.f}, {110.f, 110.f, 980.f, 980.f},
                                                     {100, 100, 1000, 1000}, 254.f, 254.f);
    utassert(clipping.pageBoundsOutsidePrintable && !clipping.contentOutsidePrintable);
    utassert(NearPreviewValue(clipping.leftMm, 1.f));
    utassert(NearPreviewValue(clipping.rightMm, 2.f));
    utassert(NearPreviewValue(clipping.topMm, 2.f));
    utassert(NearPreviewValue(clipping.bottomMm, 2.f));

    clipping = CalculatePrintClipping({80.f, 60.f, 1040.f, 1060.f}, {90.f, 70.f, 1030.f, 1050.f},
                                      {100, 100, 1000, 1000}, 254.f, 254.f);
    utassert(clipping.pageBoundsOutsidePrintable && clipping.contentOutsidePrintable);
    utassert(NearPreviewValue(clipping.leftMm, 1.f));
    utassert(NearPreviewValue(clipping.rightMm, 2.f));
    utassert(NearPreviewValue(clipping.topMm, 3.f));
    utassert(NearPreviewValue(clipping.bottomMm, 2.f));

    PrintDialogState state;
    state.previewGeneration = 7;
    InvalidatePreview(state, PreviewInvalidation::None);
    utassert(state.previewGeneration == 7);
    InvalidatePreview(state, PreviewInvalidation::Page);
    utassert(state.previewGeneration == 8);
    InvalidatePreview(state, PreviewInvalidation::Printer);
    utassert(state.previewGeneration == 9);
    state.previewGeneration = (u32)-1;
    InvalidatePreview(state, PreviewInvalidation::Page);
    utassert(state.previewGeneration == 1);
}

#endif
