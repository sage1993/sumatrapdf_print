/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

enum class PrintRotationAdv;
struct Print_Advanced_Data;

enum class PrintScaleMode {
    LegacyNone,
    Shrink,
    Fit,
    Stretch,
    Actual,
    Custom,
};

struct PrintLayoutOptions {
    PrintScaleMode scale = PrintScaleMode::Shrink;
    float customScalePercent = 100.f;
    PrintRotationAdv rotation{};
    bool autoRotate = true;
    bool centerHorizontally = false;
    int extraRotation = 0;
};

struct PrintPlacementInput {
    SizeF pageSize;
    RectF contentBox;
    Size paperSize;
    Rect printable;
    float dpiX = 96.f;
    float dpiY = 96.f;
    float fileDpi = 96.f;
    int rotation = 0;
    PrintLayoutOptions options;
};

struct PrintPageLayout {
    float zoom = 1.f;
    float physicalScale = 1.f;
    int rotation = 0;
    Point offset;
    Rect stretch;
    Rect target;
    Size renderedPageSize;
    bool isStretch = false;
};

PrintLayoutOptions PrintLayoutOptionsFromAdvanced(const Print_Advanced_Data& advanced);
int ResolvePrintRotation(SizeF& pageSize, const PrintLayoutOptions& options, bool printPortrait);
PrintPageLayout CalculatePrintPlacement(const PrintPlacementInput& input);

#if IS_DEBUG
void PrintLayout_UnitTests();
#endif
