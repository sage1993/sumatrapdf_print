/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct Gfx;
struct PrinterMetrics;
struct PrintPageLayout;
struct ClippingReport;
enum class PreviewInvalidation;

struct PrintPreviewRenderer {
    void* data = nullptr;

    PrintPreviewRenderer() = default;
    PrintPreviewRenderer(const PrintPreviewRenderer&) = delete;
    PrintPreviewRenderer& operator=(const PrintPreviewRenderer&) = delete;
    ~PrintPreviewRenderer();

    static PrintPreviewRenderer* Create(EngineBase* engine, const Func0& onPageReady, i64 maxBytes = 128 * 1024 * 1024);

    void Invalidate(PreviewInvalidation level);
    void RequestPage(int pageNo, int pageCount, const PrinterMetrics& metrics, const PrintPageLayout& layout,
                     const Rect& viewport);
    bool DrawPage(Gfx* gfx, int pageNo, const PrinterMetrics& metrics, const PrintPageLayout& layout,
                  const ClippingReport& clipping, const Rect& viewport);
    i64 CacheBytes() const;
};
