enum class PrintPageMode {
    All,
    Current,
    Range
};
enum class PrintOrientationMode {
    Auto,
    Portrait,
    Landscape
};
enum class PreviewInvalidation {
    None,
    Page,
    Printer
};

struct PrinterMetrics {
    Size paperPx;
    Rect printablePx;
    float dpiX = 0.f;
    float dpiY = 0.f;
    SizeF paperMm;
    RectF printableMm;
    WORD paperId = 0;
    bool portrait = true;
    bool duplexSupported = false;
};

struct ClippingReport {
    bool pageBoundsOutsidePrintable = false;
    bool contentOutsidePrintable = false;
    float leftMm = 0.f;
    float rightMm = 0.f;
    float topMm = 0.f;
    float bottomMm = 0.f;
};

struct PrintDialogState {
    int pageCount = 0;
    int currentDocumentPage = 1;
    int previewPage = 1;
    int copies = 1;
    PrintPageMode pageMode = PrintPageMode::All;
    Vec<PRINTPAGERANGE> ranges;
    PrintOrientationMode orientation = PrintOrientationMode::Auto;
    PrintLayoutOptions layout;
    Print_Advanced_Data advanced;
    bool duplex = false;
    u32 previewGeneration = 1;
};

bool ParsePrintRanges(Str text, int pageCount, Vec<PRINTPAGERANGE>& ranges);
bool BuildPrinterMetrics(Size paperPx, Rect printablePx, float dpiX, float dpiY, WORD paperId, bool portrait,
                         bool duplexSupported, PrinterMetrics& out);
ClippingReport CalculatePrintClipping(RectF pageTarget, RectF contentTarget, Rect printable, float dpiX, float dpiY);
void InvalidatePreview(PrintDialogState& state, PreviewInvalidation level);

#if IS_DEBUG
void PrintPreviewModel_UnitTests();
#endif
