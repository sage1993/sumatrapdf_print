/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#if IS_DEBUG
#include "base/UtAssert.h"
#endif

#include "SumatraDialogs.h"
#include "PrintLayout.h"

static bool IsPositiveFinite(float value) {
    return value > 0.f && isfinite(value);
}

static float SafeDiv(float num, float den) {
    if (!isfinite(num) || !IsPositiveFinite(den)) {
        return 0.f;
    }
    float value = num / den;
    return isfinite(value) ? value : 0.f;
}

struct PrintDpiFactors {
    float x = 1.f;
    float y = 1.f;
    float raster = 1.f;
};

static PrintDpiFactors GetDpiFactors(float dpiX, float dpiY, float fileDpi) {
    if (!IsPositiveFinite(fileDpi)) {
        fileDpi = 96.f;
    }
    if (!IsPositiveFinite(dpiX) || !IsPositiveFinite(dpiY)) {
        dpiX = fileDpi;
        dpiY = fileDpi;
    }

    PrintDpiFactors factors;
    factors.x = SafeDiv(dpiX, fileDpi);
    factors.y = SafeDiv(dpiY, fileDpi);
    factors.raster = std::min(factors.x, factors.y);
    if (!IsPositiveFinite(factors.x) || !IsPositiveFinite(factors.y) || !IsPositiveFinite(factors.raster)) {
        return {};
    }
    return factors;
}

static int RoundPx(float value) {
    if (!isfinite(value) || value <= 0.f) {
        return 0;
    }
    return (int)lroundf(value);
}

PrintLayoutOptions PrintLayoutOptionsFromAdvanced(const Print_Advanced_Data& advanced) {
    PrintLayoutOptions options;
    if (advanced.scale == PrintScaleAdv::None) {
        options.scale = PrintScaleMode::LegacyNone;
    } else if (advanced.scale == PrintScaleAdv::Fit) {
        options.scale = PrintScaleMode::Fit;
    } else if (advanced.scale == PrintScaleAdv::Stretch) {
        options.scale = PrintScaleMode::Stretch;
    } else {
        options.scale = PrintScaleMode::Shrink;
    }
    options.rotation = advanced.rotation;
    options.autoRotate = advanced.autoRotate;
    options.centerHorizontally = advanced.centerHorizontally;
    options.extraRotation = advanced.extraRotation;
    return options;
}

int ResolvePrintRotation(SizeF& pageSize, const PrintLayoutOptions& options, bool printPortrait) {
    int rotation = 0;
    if (options.autoRotate && pageSize.dx > pageSize.dy) {
        rotation += 90;
        std::swap(pageSize.dx, pageSize.dy);
    }
    rotation = (rotation % 180) == 0 ? 0 : 270;
    if (!printPortrait) {
        rotation = (rotation + 90) % 360;
        std::swap(pageSize.dx, pageSize.dy);
    }
    if (options.extraRotation != 0) {
        rotation = (rotation + options.extraRotation) % 360;
        if (options.extraRotation == 90 || options.extraRotation == 270) {
            std::swap(pageSize.dx, pageSize.dy);
        }
    }
    return rotation;
}

static float RequestedScale(const PrintLayoutOptions& options) {
    if (options.scale != PrintScaleMode::Custom) {
        return 1.f;
    }
    float scale = options.customScalePercent / 100.f;
    return IsPositiveFinite(scale) ? scale : 1.f;
}

PrintPageLayout CalculatePrintPlacement(const PrintPlacementInput& input) {
    PrintPageLayout layout;
    layout.rotation = input.rotation;

    PrintDpiFactors dpi = GetDpiFactors(input.dpiX, input.dpiY, input.fileDpi);
    float pageDx = input.pageSize.dx > 0.f ? input.pageSize.dx : 1.f;
    float pageDy = input.pageSize.dy > 0.f ? input.pageSize.dy : 1.f;
    float contentDx = input.contentBox.dx > 0.f ? input.contentBox.dx : pageDx;
    float contentDy = input.contentBox.dy > 0.f ? input.contentBox.dy : pageDy;

    if (input.options.scale == PrintScaleMode::Actual || input.options.scale == PrintScaleMode::Custom) {
        float scale = RequestedScale(input.options);
        layout.zoom = dpi.raster * scale;
        layout.physicalScale = scale;
        layout.renderedPageSize = {RoundPx(pageDx * layout.zoom), RoundPx(pageDy * layout.zoom)};
        int targetDx = RoundPx(pageDx * dpi.x * scale);
        int targetDy = RoundPx(pageDy * dpi.y * scale);
        int targetX = (input.paperSize.dx - targetDx) / 2;
        int targetY = (input.paperSize.dy - targetDy) / 2;
        layout.target = {targetX, targetY, targetDx, targetDy};
        layout.offset = {targetX - input.printable.x, targetY - input.printable.y};
        return layout;
    }

    layout.zoom = dpi.raster;
    layout.offset = {-input.printable.x, -input.printable.y};
    layout.isStretch = input.options.scale == PrintScaleMode::Stretch;

    if (layout.isStretch) {
        layout.zoom = std::max(SafeDiv((float)input.printable.dx, pageDx), SafeDiv((float)input.printable.dy, pageDy));
        if (!IsPositiveFinite(layout.zoom)) {
            layout.zoom = dpi.raster;
        }
        layout.physicalScale = 0.f;
        layout.offset = {0, 0};
        layout.stretch = {0, 0, input.printable.dx, input.printable.dy};
        layout.target = input.printable;
        layout.renderedPageSize = {RoundPx(pageDx * layout.zoom), RoundPx(pageDy * layout.zoom)};
        return layout;
    }

    if (input.options.scale != PrintScaleMode::LegacyNone) {
        layout.zoom = std::min(
            SafeDiv((float)input.printable.dx, contentDx),
            std::min(SafeDiv((float)input.printable.dy, contentDy),
                     std::min(SafeDiv((float)input.paperSize.dx, pageDx), SafeDiv((float)input.paperSize.dy, pageDy))));
        if (!IsPositiveFinite(layout.zoom)) {
            layout.zoom = dpi.raster;
        }
        if (input.options.scale == PrintScaleMode::Shrink && dpi.raster < layout.zoom) {
            layout.zoom = dpi.raster;
        }

        layout.offset.x += (int)((float)input.paperSize.dx - (pageDx * layout.zoom)) / 2;
        layout.offset.y += (int)((float)input.paperSize.dy - (pageDy * layout.zoom)) / 2;

        RectF onPaper((float)input.printable.x + (float)layout.offset.x + (input.contentBox.x * layout.zoom),
                      (float)input.printable.y + (float)layout.offset.y + (input.contentBox.y * layout.zoom),
                      input.contentBox.dx * layout.zoom, input.contentBox.dy * layout.zoom);
        if (onPaper.x < (float)input.printable.x) {
            layout.offset.x += (int)((float)input.printable.x - onPaper.x);
        } else if (onPaper.BR().x > (float)input.printable.BR().x) {
            layout.offset.x -= (int)(onPaper.BR().x - (float)input.printable.BR().x);
        }
        if (onPaper.y < (float)input.printable.y) {
            layout.offset.y += (int)((float)input.printable.y - onPaper.y);
        } else if (onPaper.BR().y > (float)input.printable.BR().y) {
            layout.offset.y -= (int)(onPaper.BR().y - (float)input.printable.BR().y);
        }
    }

    if (!IsPositiveFinite(layout.zoom)) {
        layout.zoom = IsPositiveFinite(dpi.raster) ? dpi.raster : 1.f;
    }
    layout.physicalScale = IsPositiveFinite(dpi.raster) ? layout.zoom / dpi.raster : 1.f;
    layout.renderedPageSize = {RoundPx(pageDx * layout.zoom), RoundPx(pageDy * layout.zoom)};
    if (input.options.centerHorizontally && input.options.scale == PrintScaleMode::LegacyNone) {
        layout.offset.x += (int)((float)input.paperSize.dx - (pageDx * layout.zoom)) / 2;
    }
    layout.target = {input.printable.x + layout.offset.x, input.printable.y + layout.offset.y,
                     layout.renderedPageSize.dx, layout.renderedPageSize.dy};
    return layout;
}

#if IS_DEBUG

static bool Near(float a, float b, float eps = 0.001f) {
    return fabsf(a - b) <= eps;
}

static PrintPlacementInput BasePlacement(PrintScaleMode mode) {
    PrintPlacementInput in;
    in.pageSize = {384.f, 192.f}; // 4 x 2 inches at 96 file DPI
    in.contentBox = {0.f, 0.f, 384.f, 192.f};
    in.paperSize = {5100, 6600}; // Letter at 600 DPI
    in.printable = {150, 150, 4800, 6300};
    in.dpiX = 600.f;
    in.dpiY = 600.f;
    in.fileDpi = 96.f;
    in.options.scale = mode;
    return in;
}

void PrintLayout_UnitTests() {
    {
        auto in = BasePlacement(PrintScaleMode::Actual);
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 6.25f));
        utassert(Near(got.physicalScale, 1.f));
        utassert(got.target.x == 1350 && got.target.y == 2700 && got.target.dx == 2400 && got.target.dy == 1200);
        utassert(got.renderedPageSize.dx == 2400 && got.renderedPageSize.dy == 1200);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Custom);
        in.options.customScalePercent = 50.f;
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 3.125f));
        utassert(Near(got.physicalScale, 0.5f));
        utassert(got.target.x == 1950 && got.target.y == 3000 && got.target.dx == 1200 && got.target.dy == 600);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Custom);
        in.options.customScalePercent = 150.f;
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 9.375f));
        utassert(Near(got.physicalScale, 1.5f));
        utassert(got.target.x == 750 && got.target.y == 2400 && got.target.dx == 3600 && got.target.dy == 1800);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Fit);
        in.pageSize = {960.f, 1200.f};
        in.contentBox = {0.f, 0.f, 960.f, 1200.f};
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 5.f));
        utassert(got.target.x == 150 && got.target.y == 300 && got.target.dx == 4800 && got.target.dy == 6000);
        utassert(Near(got.physicalScale, 0.8f));
    }
    {
        auto in = BasePlacement(PrintScaleMode::Shrink);
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 6.25f));
        utassert(got.target.x == 1350 && got.target.y == 2700 && got.target.dx == 2400 && got.target.dy == 1200);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Stretch);
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(got.isStretch);
        utassert(Near(got.physicalScale, 0.f));
        utassert(got.target.x == 150 && got.target.y == 150 && got.target.dx == 4800 && got.target.dy == 6300);
        utassert(got.stretch.x == 0 && got.stretch.y == 0 && got.stretch.dx == 4800 && got.stretch.dy == 6300);
    }
    {
        auto in = BasePlacement(PrintScaleMode::LegacyNone);
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 6.25f));
        utassert(got.target.x == 0 && got.target.y == 0 && got.target.dx == 2400 && got.target.dy == 1200);
        utassert(got.offset.x == -150 && got.offset.y == -150);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Actual);
        in.dpiY = 1200.f;
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 6.25f));
        utassert(Near(got.physicalScale, 1.f));
        utassert(got.renderedPageSize.dx == 2400 && got.renderedPageSize.dy == 1200);
        utassert(got.target.x == 1350 && got.target.y == 2100 && got.target.dx == 2400 && got.target.dy == 2400);
        utassert(got.target.dy != got.renderedPageSize.dy);
    }
    {
        auto in = BasePlacement(PrintScaleMode::Actual);
        in.dpiX = 0.f;
        in.dpiY = 1200.f;
        PrintPageLayout got = CalculatePrintPlacement(in);
        utassert(Near(got.zoom, 1.f));
        utassert(Near(got.physicalScale, 1.f));
        utassert(got.renderedPageSize.dx == 384 && got.renderedPageSize.dy == 192);
        utassert(got.target.dx == 384 && got.target.dy == 192);
    }
    {
        Print_Advanced_Data advanced;
        advanced.scale = PrintScaleAdv::Fit;
        advanced.rotation = PrintRotationAdv::Landscape;
        advanced.autoRotate = false;
        advanced.centerHorizontally = true;
        advanced.extraRotation = 180;
        PrintLayoutOptions got = PrintLayoutOptionsFromAdvanced(advanced);
        utassert(got.scale == PrintScaleMode::Fit);
        utassert(got.rotation == PrintRotationAdv::Landscape);
        utassert(!got.autoRotate);
        utassert(got.centerHorizontally);
        utassert(got.extraRotation == 180);
    }
    {
        PrintLayoutOptions options;
        SizeF pageSize(384.f, 192.f);
        int rotation = ResolvePrintRotation(pageSize, options, true);
        utassert(rotation == 270);
        utassert(Near(pageSize.dx, 192.f));
        utassert(Near(pageSize.dy, 384.f));
    }
}

#endif
