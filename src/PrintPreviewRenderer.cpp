/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "gui/Gfx.h"
#include "gui/UIModels.h"

#include "EngineBase.h"
#include "PageRenderPolicy.h"
#include "PageRenderService.h"
#include "SumatraDialogs.h"
#include "PrintLayout.h"
#include "PrintPreviewModel.h"
#include "PrintPreviewRenderer.h"

constexpr int kPreviewMargin = 16;
constexpr int kPreviewShadowOffset = 4;
constexpr float kPreviewOversample = 1.5f;
constexpr float kPreviewMinDpi = 96.f;
constexpr float kPreviewMaxDpi = 192.f;

constexpr Color kPreviewBackground = MkRgb(224, 224, 224);
constexpr Color kPreviewShadow = MkRgb(160, 160, 160);
constexpr Color kPreviewPaper = MkRgb(255, 255, 255);
constexpr Color kPreviewNonPrintable = MkRgb(246, 246, 246);
constexpr Color kPreviewPaperBorder = MkRgb(128, 128, 128);
constexpr Color kPreviewPrintableBorder = MkRgb(150, 150, 150);
constexpr Color kPreviewPageClip = MkRgb(220, 140, 20);
constexpr Color kPreviewContentClip = MkRgb(200, 40, 40);

struct PrintPreviewRendererData {
    PageRenderService* service = nullptr;
    float fileDpi = 96.f;
};

struct PreviewGeometry {
    Rect paper;
    Rect printable;
    float scaleX = 0.f;
    float scaleY = 0.f;
    float rasterScale = 0.f;
};

static PrintPreviewRendererData* RendererData(PrintPreviewRenderer* renderer) {
    return (PrintPreviewRendererData*)renderer->data;
}

static bool PositiveFinite(float value) {
    return value > 0.f && isfinite(value);
}

static int PreviewRound(float value) {
    return (int)lroundf(value);
}

static Rect MapPrinterRect(const Rect& rect, const PreviewGeometry& geometry) {
    Rect mapped;
    mapped.x = geometry.paper.x + PreviewRound((float)rect.x * geometry.scaleX);
    mapped.y = geometry.paper.y + PreviewRound((float)rect.y * geometry.scaleY);
    mapped.dx = std::max(1, PreviewRound((float)rect.dx * geometry.scaleX));
    mapped.dy = std::max(1, PreviewRound((float)rect.dy * geometry.scaleY));
    return mapped;
}

static bool BuildPreviewGeometry(const PrinterMetrics& metrics, const Rect& viewport, PreviewGeometry& geometry) {
    geometry = {};
    if (metrics.paperPx.dx <= 0 || metrics.paperPx.dy <= 0 || !PositiveFinite(metrics.dpiX) ||
        !PositiveFinite(metrics.dpiY) || viewport.dx <= 0 || viewport.dy <= 0) {
        return false;
    }

    float paperInchesX = (float)metrics.paperPx.dx / metrics.dpiX;
    float paperInchesY = (float)metrics.paperPx.dy / metrics.dpiY;
    if (!PositiveFinite(paperInchesX) || !PositiveFinite(paperInchesY)) {
        return false;
    }

    int availableDx = std::max(1, viewport.dx - (2 * kPreviewMargin) - kPreviewShadowOffset);
    int availableDy = std::max(1, viewport.dy - (2 * kPreviewMargin) - kPreviewShadowOffset);
    float viewDpi = std::min((float)availableDx / paperInchesX, (float)availableDy / paperInchesY);
    if (!PositiveFinite(viewDpi)) {
        return false;
    }

    geometry.scaleX = viewDpi / metrics.dpiX;
    geometry.scaleY = viewDpi / metrics.dpiY;
    geometry.rasterScale = metrics.dpiX <= metrics.dpiY ? geometry.scaleX : geometry.scaleY;

    int paperDx = std::max(1, PreviewRound((float)metrics.paperPx.dx * geometry.scaleX));
    int paperDy = std::max(1, PreviewRound((float)metrics.paperPx.dy * geometry.scaleY));
    geometry.paper = {viewport.x + (viewport.dx - kPreviewShadowOffset - paperDx) / 2,
                      viewport.y + (viewport.dy - kPreviewShadowOffset - paperDy) / 2, paperDx, paperDy};
    geometry.printable = MapPrinterRect(metrics.printablePx, geometry);
    return true;
}

static float PreviewRasterZoom(float layoutZoom, float paperToViewScale, float fileDpi) {
    if (!PositiveFinite(fileDpi)) {
        fileDpi = 96.f;
    }

    float minZoom = kPreviewMinDpi / fileDpi;
    float maxZoom = kPreviewMaxDpi / fileDpi;
    float zoom = layoutZoom * paperToViewScale * kPreviewOversample;
    if (!PositiveFinite(zoom)) {
        return minZoom;
    }
    return std::max(minZoom, std::min(zoom, maxZoom));
}

static PageRenderKey PreviewKey(int pageNo, const PrintPageLayout& layout, const PreviewGeometry& geometry,
                                float fileDpi) {
    PageRenderKey key;
    key.pageNo = pageNo;
    key.zoom = PreviewRasterZoom(layout.zoom, geometry.rasterScale, fileDpi);
    key.rotation = layout.rotation;
    return key;
}

PrintPreviewRenderer* PrintPreviewRenderer::Create(EngineBase* engine, const Func0& onPageReady, i64 maxBytes) {
    PageRenderService* service = PageRenderService::CreateForPrint(engine, onPageReady, maxBytes);
    if (!service) {
        return nullptr;
    }

    auto* renderer = new PrintPreviewRenderer();
    auto* rendererData = new PrintPreviewRendererData();
    renderer->data = rendererData;
    rendererData->service = service;
    float fileDpi = engine->GetFileDPI();
    rendererData->fileDpi = PositiveFinite(fileDpi) ? fileDpi : 96.f;
    return renderer;
}

PrintPreviewRenderer::~PrintPreviewRenderer() {
    auto* rendererData = RendererData(this);
    if (!rendererData) {
        return;
    }
    delete rendererData->service;
    delete rendererData;
    data = nullptr;
}

void PrintPreviewRenderer::Invalidate(PreviewInvalidation level) {
    if (level == PreviewInvalidation::None) {
        return;
    }
    auto* rendererData = RendererData(this);
    if (rendererData && rendererData->service) {
        rendererData->service->NewGeneration();
    }
}

void PrintPreviewRenderer::RequestPage(int pageNo, int pageCount, const PrinterMetrics& metrics,
                                       const PrintPageLayout& layout, const Rect& viewport) {
    auto* rendererData = RendererData(this);
    if (!rendererData || !rendererData->service || pageNo < 1 || pageNo > pageCount) {
        return;
    }

    PreviewGeometry geometry;
    if (!BuildPreviewGeometry(metrics, viewport, geometry)) {
        return;
    }

    PageRenderKey key = PreviewKey(pageNo, layout, geometry, rendererData->fileDpi);
    rendererData->service->Request(key, PageRenderPriority::Visible);
    if (pageNo > 1) {
        key.pageNo = pageNo - 1;
        rendererData->service->Request(key, PageRenderPriority::Nearby);
    }
    if (pageNo < pageCount) {
        key.pageNo = pageNo + 1;
        rendererData->service->Request(key, PageRenderPriority::Nearby);
    }
}

bool PrintPreviewRenderer::DrawPage(Gfx* gfx, int pageNo, const PrinterMetrics& metrics, const PrintPageLayout& layout,
                                    const ClippingReport& clipping, const Rect& viewport) {
    auto* rendererData = RendererData(this);
    if (!gfx || !rendererData || !rendererData->service) {
        return false;
    }

    PreviewGeometry geometry;
    if (!BuildPreviewGeometry(metrics, viewport, geometry)) {
        return false;
    }

    gfx->FillRect(viewport, kPreviewBackground);
    Rect shadow = geometry.paper;
    shadow.x += kPreviewShadowOffset;
    shadow.y += kPreviewShadowOffset;
    gfx->FillRect(shadow, kPreviewShadow);
    gfx->FillRect(geometry.paper, kPreviewNonPrintable);
    gfx->FillRect(geometry.printable, kPreviewPaper);
    gfx->DrawRect(geometry.paper, kPreviewPaperBorder);
    gfx->DrawDashedRect(geometry.printable, kPreviewPrintableBorder);

    PageRenderKey key = PreviewKey(pageNo, layout, geometry, rendererData->fileDpi);
    Rect pageTarget = MapPrinterRect(layout.target, geometry);
    gfx->PushClip(geometry.paper);
    bool drawn = rendererData->service->DrawPage(gfx, key, pageTarget);
    gfx->PopClip();

    if (clipping.contentOutsidePrintable) {
        gfx->DrawRect(geometry.printable, kPreviewContentClip, 2);
    } else if (clipping.pageBoundsOutsidePrintable) {
        gfx->DrawRect(geometry.printable, kPreviewPageClip, 2);
    }
    return drawn;
}

i64 PrintPreviewRenderer::CacheBytes() const {
    auto* rendererData = RendererData((PrintPreviewRenderer*)this);
    if (!rendererData || !rendererData->service) {
        return 0;
    }
    return rendererData->service->CacheBytes();
}
