/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "gui/UIModels.h"
#include "gui/Gfx.h"

#include "Settings.h"
#include "EngineBase.h"
#include "MainWindow.h"
#include "Print.h"
#include "SumatraDialogs.h"
#include "PrintLayout.h"
#include "PrintPreviewPrinter.h"
#include "PrintPreviewRenderer.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "Theme.h"
#include "DarkMode_win.h"
#include "WindowTab.h"
#include "resource.h"
#include "PrintPreviewDialog.h"

struct PrintPreviewDialogData {
    HWND hwnd = nullptr;
    MainWindow* win = nullptr;
    EngineBase* engine = nullptr;
    PrinterSession* session = nullptr;
    PrintPreviewRenderer* renderer = nullptr;
    PrintDialogState state;
    bool rangeValid = true;
    bool closing = false;
};

static PrintPreviewDialogData* DialogData(HWND hwnd) {
    return (PrintPreviewDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void SetRadio(HWND hwnd, int first, int last, int selected) {
    CheckRadioButton(hwnd, first, last, selected);
}

static PrintPageLayout CurrentLayout(PrintPreviewDialogData* data, int pageNo) {
    return CalculatePrintPageLayout(*data->engine, pageNo, data->state.layout, data->session->metrics.paperPx,
                                    data->session->metrics.printablePx, data->session->metrics.dpiX,
                                    data->session->metrics.dpiY, data->session->metrics.portrait,
                                    data->session->printer->name);
}

static RectF OffsetRect(RectF rect, Point offset) {
    rect.x += (float)offset.x;
    rect.y += (float)offset.y;
    return rect;
}

static ClippingReport CurrentClipping(PrintPreviewDialogData* data, int pageNo, const PrintPageLayout& layout) {
    RectF page = data->engine->PageMediabox(pageNo);
    RectF content = data->engine->PageContentBox(pageNo, RenderTarget::Print);
    if (content.IsEmpty() || content.dx <= 0.f || content.dy <= 0.f) {
        content = page;
    }

    page = data->engine->Transform(page, pageNo, layout.zoom, layout.rotation);
    content = data->engine->Transform(content, pageNo, layout.zoom, layout.rotation);
    page = OffsetRect(page, layout.offset);
    content = OffsetRect(content, layout.offset);
    return CalculatePrintClipping(page, content, data->session->metrics.printablePx, data->session->metrics.dpiX,
                                  data->session->metrics.dpiY);
}

static void UpdatePageIndicator(PrintPreviewDialogData* data) {
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_INDICATOR,
                       fmt(_TRA("Page %d of %d").s, data->state.previewPage, data->state.pageCount));
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_PREV), data->state.previewPage > 1);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_NEXT), data->state.previewPage < data->state.pageCount);
}

static void UpdatePrinterInfo(PrintPreviewDialogData* data) {
    if (!data->session) {
        HwndSetDlgItemText(data->hwnd, IDC_PP_DOC_SIZE, StrL(""));
        HwndSetDlgItemText(data->hwnd, IDC_PP_PAPER_SIZE, StrL(""));
        HwndSetDlgItemText(data->hwnd, IDC_PP_PRINTABLE_SIZE, StrL(""));
        return;
    }

    const PrinterMetrics& metrics = data->session->metrics;
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAPER_SIZE,
                       fmt(_TRA("Paper: %.1f x %.1f mm").s, metrics.paperMm.dx, metrics.paperMm.dy));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINTABLE_SIZE,
                       fmt(_TRA("Printable: %.1f x %.1f mm").s, metrics.printableMm.dx, metrics.printableMm.dy));
    HwndSetDlgItemText(data->hwnd, IDC_PP_DOC_SIZE,
                       fmt(_TRA("Document: %.0f x %.0f px").s, data->engine->PageMediabox(data->state.previewPage).dx,
                           data->engine->PageMediabox(data->state.previewPage).dy));
}

static void UpdateClippingInfo(PrintPreviewDialogData* data) {
    if (!data->session || data->state.pageCount < 1) {
        HwndSetDlgItemText(data->hwnd, IDC_PP_CLIPPING, StrL(""));
        return;
    }

    PrintPageLayout layout = CurrentLayout(data, data->state.previewPage);
    ClippingReport clipping = CurrentClipping(data, data->state.previewPage, layout);
    float scale = layout.physicalScale * 100.f;
    if (!isfinite(scale) || scale <= 0.f) {
        scale = 100.f;
    }
    HwndSetDlgItemText(data->hwnd, IDC_PP_EFFECTIVE_SCALE, fmt(_TRA("Scale: %.1f%%").s, scale));
    if (clipping.pageBoundsOutsidePrintable || clipping.contentOutsidePrintable) {
        HwndSetDlgItemText(data->hwnd, IDC_PP_CLIPPING,
                           fmt(_TRA("Clipping: L %.1f  R %.1f  T %.1f  B %.1f mm").s, clipping.leftMm, clipping.rightMm,
                               clipping.topMm, clipping.bottomMm));
    } else {
        HwndSetDlgItemText(data->hwnd, IDC_PP_CLIPPING, _TRA("Clipping: none"));
    }
    HwndSetDlgItemText(data->hwnd, IDC_PP_OUTPUT_SIZE,
                       fmt(_TRA("Output: %d x %d px").s, layout.target.dx, layout.target.dy));
}

static void UpdateControls(PrintPreviewDialogData* data) {
    UpdatePageIndicator(data);
    UpdatePrinterInfo(data);
    UpdateClippingInfo(data);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_PROPERTIES), data->session != nullptr);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_PAGE_SETUP), data->session != nullptr);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_ADVANCED), data->session != nullptr);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_PRINT), data->session != nullptr && data->rangeValid);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_SYSTEM_PRINT), data->session != nullptr);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_DUPLEX), data->session && data->session->metrics.duplexSupported);
    CheckDlgButton(data->hwnd, IDC_PP_DUPLEX, data->state.duplex ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_SCALE_CUSTOM_EDIT), data->state.layout.scale == PrintScaleMode::Custom);
}

static void InvalidatePreviewState(PrintPreviewDialogData* data, PreviewInvalidation level) {
    InvalidatePreview(data->state, level);
    if (data->renderer) {
        data->renderer->Invalidate(level);
    }
    UpdateControls(data);
    InvalidateRect(GetDlgItem(data->hwnd, IDC_PP_PREVIEW), nullptr, FALSE);
}

static void SetScale(PrintPreviewDialogData* data, PrintScaleMode mode) {
    data->state.layout.scale = mode;
    if (mode == PrintScaleMode::LegacyNone || mode == PrintScaleMode::Actual) {
        data->state.advanced.scale = PrintScaleAdv::None;
    } else if (mode == PrintScaleMode::Fit) {
        data->state.advanced.scale = PrintScaleAdv::Fit;
    } else if (mode == PrintScaleMode::Stretch) {
        data->state.advanced.scale = PrintScaleAdv::Stretch;
    } else {
        data->state.advanced.scale = PrintScaleAdv::Shrink;
    }
    InvalidatePreviewState(data, PreviewInvalidation::Page);
}

static void SetRangesForMode(PrintPreviewDialogData* data) {
    VecClear(data->state.ranges);
    if (data->state.pageMode == PrintPageMode::All) {
        PRINTPAGERANGE range{1, (DWORD)data->state.pageCount};
        VecAppend(data->state.ranges, range);
        data->rangeValid = true;
        return;
    }
    if (data->state.pageMode == PrintPageMode::Current) {
        PRINTPAGERANGE range{(DWORD)data->state.currentDocumentPage, (DWORD)data->state.currentDocumentPage};
        VecAppend(data->state.ranges, range);
        data->rangeValid = true;
        return;
    }

    data->rangeValid = ParsePrintRanges(HwndGetTextTemp(GetDlgItem(data->hwnd, IDC_PP_PAGE_RANGE_EDIT)),
                                        data->state.pageCount, data->state.ranges);
}

static void UpdateRangeControls(PrintPreviewDialogData* data) {
    SetRadio(data->hwnd, IDC_PP_PAGE_ALL, IDC_PP_PAGE_RANGE,
             data->state.pageMode == PrintPageMode::All       ? IDC_PP_PAGE_ALL
             : data->state.pageMode == PrintPageMode::Current ? IDC_PP_PAGE_CURRENT
                                                              : IDC_PP_PAGE_RANGE);
    EnableWindow(GetDlgItem(data->hwnd, IDC_PP_PAGE_RANGE_EDIT), data->state.pageMode == PrintPageMode::Range);
    SetRangesForMode(data);
    UpdateControls(data);
}

static void FillPrinterControls(PrintPreviewDialogData* data) {
    StrVec names;
    GetPrinterNames(names);
    HWND combo = GetDlgItem(data->hwnd, IDC_PP_PRINTER);
    CbResetContent(combo);
    int selected = -1;
    for (int i = 0; i < len(names); i++) {
        CbAddString(combo, names[i]);
        if (data->session && str::EqI(names[i], data->session->printer->name)) {
            selected = i;
        }
    }
    if (selected >= 0) {
        CbSetCurrentSelection(combo, selected);
    } else if (len(names) > 0) {
        CbSetCurrentSelection(combo, 0);
    }
}

static void FillPaperControls(PrintPreviewDialogData* data) {
    HWND combo = GetDlgItem(data->hwnd, IDC_PP_SETUP_PAPER);
    CbResetContent(combo);
    if (!data->session || !data->session->printer) {
        return;
    }
    int selected = 0;
    for (int i = 0; i < data->session->printer->nPaperSizes; i++) {
        CbAddString(combo, data->session->printer->paperNames[i]);
        if (data->session->printer->papers[i] == data->session->metrics.paperId) {
            selected = i;
        }
    }
    CbSetCurrentSelection(combo, selected);
}

static void FillBinControls(PrintPreviewDialogData* data) {
    HWND combo = GetDlgItem(data->hwnd, IDC_PP_SETUP_BIN);
    CbResetContent(combo);
    if (!data->session || !data->session->printer) {
        return;
    }
    for (int i = 0; i < data->session->printer->nBins; i++) {
        CbAddString(combo, data->session->printer->binNames[i]);
    }
    if (data->session->printer->nBins > 0) {
        int selected = 0;
        WORD current = data->session->printer->devMode->dmDefaultSource;
        for (int i = 0; i < data->session->printer->nBins; i++) {
            if (data->session->printer->bins[i] == current) {
                selected = i;
                break;
            }
        }
        CbSetCurrentSelection(combo, selected);
    }
}

static void SetOrientationControls(PrintPreviewDialogData* data) {
    int id = IDC_PP_ORIENT_AUTO;
    if (data->state.orientation == PrintOrientationMode::Portrait) {
        id = IDC_PP_ORIENT_PORTRAIT;
    } else if (data->state.orientation == PrintOrientationMode::Landscape) {
        id = IDC_PP_ORIENT_LANDSCAPE;
    }
    SetRadio(data->hwnd, IDC_PP_ORIENT_AUTO, IDC_PP_ORIENT_LANDSCAPE, id);
}

static void ReadDuplexState(PrintPreviewDialogData* data) {
    data->state.duplex = data->session && data->session->printer && data->session->printer->devMode &&
                         (data->session->printer->devMode->dmFields & DM_DUPLEX) &&
                         data->session->printer->devMode->dmDuplex != DMDUP_SIMPLEX;
}

static void RefreshSessionControls(PrintPreviewDialogData* data) {
    FillPrinterControls(data);
    FillPaperControls(data);
    FillBinControls(data);
    SetOrientationControls(data);
    UpdateControls(data);
}

static void PaintPreview(PrintPreviewDialogData* data, DRAWITEMSTRUCT* draw) {
    if (!data || !data->renderer || !data->session || data->state.pageCount < 1) {
        HdcFillRect(draw->hDC, ToRect(draw->rcItem), ThemeMainWindowBackgroundColor());
        return;
    }

    Rect viewport = ToRect(draw->rcItem);
    PrintPageLayout layout = CurrentLayout(data, data->state.previewPage);
    data->renderer->RequestPage(data->state.previewPage, data->state.pageCount, data->session->metrics, layout,
                                viewport);
    ClippingReport clipping = CurrentClipping(data, data->state.previewPage, layout);
    GfxHdc gfx(draw->hDC);
    data->renderer->DrawPage(&gfx, data->state.previewPage, data->session->metrics, layout, clipping, viewport);
}

struct PageSetupData {
    PrinterSession** session = nullptr;
    bool applied = false;
};

static void FillSetupControls(HWND hwnd, PrinterSession* session) {
    HWND paper = GetDlgItem(hwnd, IDC_PP_SETUP_PAPER);
    HWND bin = GetDlgItem(hwnd, IDC_PP_SETUP_BIN);
    CbResetContent(paper);
    CbResetContent(bin);
    if (!session || !session->printer) {
        return;
    }

    int paperSelection = 0;
    for (int i = 0; i < session->printer->nPaperSizes; i++) {
        CbAddString(paper, session->printer->paperNames[i]);
        if (session->printer->papers[i] == session->metrics.paperId) {
            paperSelection = i;
        }
    }
    CbSetCurrentSelection(paper, paperSelection);

    for (int i = 0; i < session->printer->nBins; i++) {
        CbAddString(bin, session->printer->binNames[i]);
    }
    if (session->printer->nBins > 0) {
        int binSelection = 0;
        WORD current = session->printer->devMode->dmDefaultSource;
        for (int i = 0; i < session->printer->nBins; i++) {
            if (session->printer->bins[i] == current) {
                binSelection = i;
                break;
            }
        }
        CbSetCurrentSelection(bin, binSelection);
    }

    int orientation = session->metrics.portrait ? IDC_PP_SETUP_PORTRAIT : IDC_PP_SETUP_LANDSCAPE;
    SetRadio(hwnd, IDC_PP_SETUP_PORTRAIT, IDC_PP_SETUP_LANDSCAPE, orientation);
}

static INT_PTR CALLBACK PageSetupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = (PageSetupData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_INITDIALOG: {
            data = (PageSetupData*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
            DarkModeApplyToWindow(hwnd);
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_PAPER, _TRA("Paper"));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_BIN, _TRA("Paper source"));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_PORTRAIT, _TRA("Portrait"));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_LANDSCAPE, _TRA("Landscape"));
            FillSetupControls(hwnd, *data->session);
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(wp) != IDOK) {
                return FALSE;
            }
            if (!*data->session || !(*data->session)->printer) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            PrinterSession* working = ClonePrinterSession(*data->session);
            if (!working) {
                return TRUE;
            }
            {
                int paper = CbGetCurrentSelection(GetDlgItem(hwnd, IDC_PP_SETUP_PAPER));
                if (paper >= 0 && paper < working->printer->nPaperSizes &&
                    !SetPrinterPaper(working, working->printer->papers[paper])) {
                    delete working;
                    return TRUE;
                }
                int bin = CbGetCurrentSelection(GetDlgItem(hwnd, IDC_PP_SETUP_BIN));
                if (bin >= 0 && bin < working->printer->nBins && !SetPrinterBin(working, working->printer->bins[bin])) {
                    delete working;
                    return TRUE;
                }
                PrintOrientationMode orientation = IsDlgButtonChecked(hwnd, IDC_PP_SETUP_PORTRAIT)
                                                       ? PrintOrientationMode::Portrait
                                                       : PrintOrientationMode::Landscape;
                if (!SetPrinterOrientation(working, orientation)) {
                    delete working;
                    return TRUE;
                }
            }
            delete *data->session;
            *data->session = working;
            data->applied = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
    }
    return FALSE;
}

static bool ShowPageSetup(HWND owner, PrinterSession*& session) {
    PageSetupData data;
    data.session = &session;
    return CreateAppDialog(IDD_PRINT_PAGE_SETUP, owner, PageSetupProc, (LPARAM)&data) == IDOK && data.applied;
}

struct AdvancedDialogData {
    Print_Advanced_Data value;
};

static INT_PTR CALLBACK AdvancedProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = (AdvancedDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_INITDIALOG: {
            data = (AdvancedDialogData*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
            DarkModeApplyToWindow(hwnd);
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_ALL, _TRA("All pages"));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_EVEN, _TRA("Even pages only"));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_ODD, _TRA("Odd pages only"));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_PER_PAGE, _TRA("Use each page's paper size"));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_ROTATE, _TRA("Rotation"));
            HWND rotate = GetDlgItem(hwnd, IDC_PP_ADV_ROTATE);
            CbAddString(rotate, _TRA("None"));
            CbAddString(rotate, StrL("90°"));
            CbAddString(rotate, StrL("180°"));
            CbAddString(rotate, StrL("270°"));
            CbSetCurrentSelection(rotate, (data->value.extraRotation / 90) % 4);
            int range = IDC_PP_ADV_RANGE_ALL;
            if (data->value.range == PrintRangeAdv::Even) {
                range = IDC_PP_ADV_RANGE_EVEN;
            } else if (data->value.range == PrintRangeAdv::Odd) {
                range = IDC_PP_ADV_RANGE_ODD;
            }
            SetRadio(hwnd, IDC_PP_ADV_RANGE_ALL, IDC_PP_ADV_RANGE_ODD, range);
            CheckDlgButton(hwnd, IDC_PP_ADV_PER_PAGE, data->value.perPagePaperSize ? BST_CHECKED : BST_UNCHECKED);
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(wp) != IDOK) {
                return FALSE;
            }
            if (IsDlgButtonChecked(hwnd, IDC_PP_ADV_RANGE_EVEN)) {
                data->value.range = PrintRangeAdv::Even;
            } else if (IsDlgButtonChecked(hwnd, IDC_PP_ADV_RANGE_ODD)) {
                data->value.range = PrintRangeAdv::Odd;
            } else {
                data->value.range = PrintRangeAdv::All;
            }
            data->value.perPagePaperSize = IsDlgButtonChecked(hwnd, IDC_PP_ADV_PER_PAGE) == BST_CHECKED;
            int rotation = CbGetCurrentSelection(GetDlgItem(hwnd, IDC_PP_ADV_ROTATE));
            data->value.extraRotation = std::max(0, rotation) * 90;
            EndDialog(hwnd, IDOK);
            return TRUE;
    }
    return FALSE;
}

static bool ShowAdvanced(HWND owner, Print_Advanced_Data& value) {
    AdvancedDialogData data;
    data.value = value;
    if (CreateAppDialog(IDD_PRINT_ADVANCED_V1, owner, AdvancedProc, (LPARAM)&data) != IDOK) {
        return false;
    }
    value = data.value;
    return true;
}

static void SelectPrinter(PrintPreviewDialogData* data) {
    HWND combo = GetDlgItem(data->hwnd, IDC_PP_PRINTER);
    TempStr name = HwndGetTextTemp(combo);
    PrinterSession* candidate = NewPrinterSession(name);
    if (!candidate) {
        MessageBoxWarning(data->hwnd, _TRA("Couldn't initialize printer"), _TRA("Printing problem."));
        return;
    }
    delete data->session;
    data->session = candidate;
    data->state.orientation = PrintOrientationMode::Auto;
    ReadDuplexState(data);
    RefreshSessionControls(data);
    InvalidatePreviewState(data, PreviewInvalidation::Printer);
}

static void ApplyAdvanced(PrintPreviewDialogData* data, const Print_Advanced_Data& advanced) {
    data->state.advanced = advanced;
    data->state.layout.rotation = advanced.rotation;
    data->state.layout.autoRotate = advanced.autoRotate;
    data->state.layout.centerHorizontally = advanced.centerHorizontally;
    data->state.layout.extraRotation = advanced.extraRotation;
    InvalidatePreviewState(data, PreviewInvalidation::Page);
}

static void OnPreviewReady(PrintPreviewDialogData* data) {
    if (!data || data->closing || !data->hwnd || !IsWindow(data->hwnd)) {
        return;
    }
    InvalidateRect(GetDlgItem(data->hwnd, IDC_PP_PREVIEW), nullptr, FALSE);
}

static void ResizePreview(HWND hwnd) {
    HWND preview = GetDlgItem(hwnd, IDC_PP_PREVIEW);
    if (!preview) {
        return;
    }
    RECT rc;
    GetWindowRect(preview, &rc);
    MapWindowPoints(nullptr, hwnd, (POINT*)&rc, 2);
    RECT client;
    GetClientRect(hwnd, &client);
    int rightMargin = std::max(1, (int)client.right - (int)rc.right);
    int bottomMargin = std::max(1, (int)client.bottom - (int)rc.bottom);
    int dx = std::max(100, (int)client.right - rightMargin - (int)rc.left);
    int dy = std::max(80, (int)client.bottom - bottomMargin - (int)rc.top);
    MoveWindow(preview, rc.left, rc.top, dx, dy, TRUE);
}

static void SetInitialControls(PrintPreviewDialogData* data) {
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINTER, _TRA("Printer"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PROPERTIES, _TRA("Properties..."));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ADVANCED, _TRA("Advanced..."));
    HwndSetDlgItemText(data->hwnd, IDC_PP_COPIES, _TRA("Copies:"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_ALL, _TRA("All"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_CURRENT, _TRA("Current page"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_RANGE, _TRA("Pages:"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_FIT, _TRA("Fit"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_ACTUAL, _TRA("Actual size"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_SHRINK, _TRA("Shrink"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_CUSTOM, _TRA("Custom:"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAPER_SOURCE_BY_SIZE, _TRA("Choose paper source by size"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_DUPLEX, _TRA("Duplex"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_AUTO, _TRA("Auto"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_PORTRAIT, _TRA("Portrait"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_LANDSCAPE, _TRA("Landscape"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PREV, _TRA("Previous"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_NEXT, _TRA("Next"));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_SETUP, _TRA("Page setup..."));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SYSTEM_PRINT, _TRA("System print..."));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINT, _TRA("Print"));
    HwndSetDlgItemText(data->hwnd, IDCANCEL, _TRA("Cancel"));
    SetDlgItemInt(data->hwnd, IDC_PP_COPIES, 1, FALSE);
    CheckDlgButton(data->hwnd, IDC_PP_PAPER_SOURCE_BY_SIZE,
                   data->state.advanced.paperSourceByPageSize ? BST_CHECKED : BST_UNCHECKED);
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_RANGE_EDIT, fmt("1-%d", data->state.pageCount));
    SetDlgItemInt(data->hwnd, IDC_PP_SCALE_CUSTOM_EDIT, 100, FALSE);

    int scale = IDC_PP_SCALE_SHRINK;
    if (data->state.layout.scale == PrintScaleMode::Fit) {
        scale = IDC_PP_SCALE_FIT;
    } else if (data->state.layout.scale == PrintScaleMode::Actual ||
               data->state.layout.scale == PrintScaleMode::LegacyNone) {
        scale = IDC_PP_SCALE_ACTUAL;
    } else if (data->state.layout.scale == PrintScaleMode::Custom) {
        scale = IDC_PP_SCALE_CUSTOM;
    }
    SetRadio(data->hwnd, IDC_PP_SCALE_FIT, IDC_PP_SCALE_CUSTOM, scale);
    UpdateRangeControls(data);
    RefreshSessionControls(data);
}

static void UpdateOrientation(PrintPreviewDialogData* data, PrintOrientationMode orientation) {
    if (!SetPrinterOrientation(data->session, orientation)) {
        return;
    }
    data->state.orientation = orientation;
    RefreshSessionControls(data);
    InvalidatePreviewState(data, PreviewInvalidation::Printer);
}

static bool ReadCustomScale(PrintPreviewDialogData* data) {
    BOOL translated = FALSE;
    UINT value = GetDlgItemInt(data->hwnd, IDC_PP_SCALE_CUSTOM_EDIT, &translated, FALSE);
    if (!translated || value < 1 || value > 1000) {
        return false;
    }
    data->state.layout.customScalePercent = (float)value;
    return true;
}

static void CopyPrintOutput(PrintPreviewDialogData* data, PrintDialogOutput& output) {
    SetRangesForMode(data);
    output.advanced = data->state.advanced;
    output.layout = data->state.layout;
    VecClear(output.ranges);
    for (const PRINTPAGERANGE& range : data->state.ranges) {
        VecAppend(output.ranges, range);
    }
    BOOL translated = FALSE;
    UINT copies = GetDlgItemInt(data->hwnd, IDC_PP_COPIES, &translated, FALSE);
    if (!translated || copies < 1) {
        copies = 1;
    }
    copies = std::min(copies, (UINT)SHRT_MAX);
    if (data->session && data->session->printer && data->session->printer->devMode) {
        data->session->printer->devMode->dmCopies = (short)copies;
        data->session->printer->devMode->dmFields |= DM_COPIES;
        if (data->session->printer->isDuplex) {
            data->session->printer->devMode->dmDuplex = data->state.duplex ? DMDUP_VERTICAL : DMDUP_SIMPLEX;
            data->session->printer->devMode->dmFields |= DM_DUPLEX;
        }
    }
    output.printer = data->session->printer;
    data->session->printer = nullptr;
}

static INT_PTR CALLBACK PrintPreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = DialogData(hwnd);
    switch (msg) {
        case WM_INITDIALOG: {
            data = (PrintPreviewDialogData*)lp;
            data->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
            DarkModeApplyToWindow(hwnd);
            SetInitialControls(data);
            return FALSE;
        }

        case WM_DRAWITEM:
            if (wp == IDC_PP_PREVIEW) {
                PaintPreview(data, (DRAWITEMSTRUCT*)lp);
                return TRUE;
            }
            return FALSE;

        case WM_SIZE:
            if (data) {
                ResizePreview(hwnd);
                InvalidateRect(GetDlgItem(hwnd, IDC_PP_PREVIEW), nullptr, FALSE);
            }
            return TRUE;

        case WM_COMMAND: {
            if (!data) {
                return FALSE;
            }
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IDC_PP_PRINTER && code == CBN_SELCHANGE) {
                SelectPrinter(data);
                return TRUE;
            }
            if (id == IDC_PP_PROPERTIES && code == BN_CLICKED) {
                PrinterPropertyResult result = ShowPrinterProperties(hwnd, data->session);
                if (result == PrinterPropertyResult::Applied) {
                    data->state.orientation = PrintOrientationMode::Auto;
                    ReadDuplexState(data);
                    RefreshSessionControls(data);
                    InvalidatePreviewState(data, PreviewInvalidation::Printer);
                }
                return TRUE;
            }
            if (id == IDC_PP_PAGE_SETUP && code == BN_CLICKED) {
                if (ShowPageSetup(hwnd, data->session)) {
                    data->state.orientation = data->session->metrics.portrait ? PrintOrientationMode::Portrait
                                                                              : PrintOrientationMode::Landscape;
                    ReadDuplexState(data);
                    RefreshSessionControls(data);
                    InvalidatePreviewState(data, PreviewInvalidation::Printer);
                }
                return TRUE;
            }
            if (id == IDC_PP_ADVANCED && code == BN_CLICKED) {
                Print_Advanced_Data advanced = data->state.advanced;
                if (ShowAdvanced(hwnd, advanced)) {
                    ApplyAdvanced(data, advanced);
                }
                return TRUE;
            }
            if ((id == IDC_PP_PAGE_ALL || id == IDC_PP_PAGE_CURRENT || id == IDC_PP_PAGE_RANGE) && code == BN_CLICKED) {
                data->state.pageMode = id == IDC_PP_PAGE_ALL       ? PrintPageMode::All
                                       : id == IDC_PP_PAGE_CURRENT ? PrintPageMode::Current
                                                                   : PrintPageMode::Range;
                UpdateRangeControls(data);
                InvalidatePreviewState(data, PreviewInvalidation::Page);
                return TRUE;
            }
            if (id == IDC_PP_PAGE_RANGE_EDIT && code == EN_CHANGE && data->state.pageMode == PrintPageMode::Range) {
                UpdateRangeControls(data);
                return TRUE;
            }
            if (id == IDC_PP_SCALE_FIT && code == BN_CLICKED) {
                SetScale(data, PrintScaleMode::Fit);
                SetRadio(hwnd, IDC_PP_SCALE_FIT, IDC_PP_SCALE_CUSTOM, id);
                return TRUE;
            }
            if (id == IDC_PP_SCALE_ACTUAL && code == BN_CLICKED) {
                SetScale(data, PrintScaleMode::Actual);
                SetRadio(hwnd, IDC_PP_SCALE_FIT, IDC_PP_SCALE_CUSTOM, id);
                return TRUE;
            }
            if (id == IDC_PP_SCALE_SHRINK && code == BN_CLICKED) {
                SetScale(data, PrintScaleMode::Shrink);
                SetRadio(hwnd, IDC_PP_SCALE_FIT, IDC_PP_SCALE_CUSTOM, id);
                return TRUE;
            }
            if (id == IDC_PP_SCALE_CUSTOM && code == BN_CLICKED) {
                SetScale(data, PrintScaleMode::Custom);
                ReadCustomScale(data);
                SetRadio(hwnd, IDC_PP_SCALE_FIT, IDC_PP_SCALE_CUSTOM, id);
                return TRUE;
            }
            if (id == IDC_PP_SCALE_CUSTOM_EDIT && code == EN_CHANGE &&
                data->state.layout.scale == PrintScaleMode::Custom) {
                if (ReadCustomScale(data)) {
                    InvalidatePreviewState(data, PreviewInvalidation::Page);
                }
                return TRUE;
            }
            if (id == IDC_PP_ORIENT_AUTO && code == BN_CLICKED) {
                UpdateOrientation(data, PrintOrientationMode::Auto);
                return TRUE;
            }
            if (id == IDC_PP_ORIENT_PORTRAIT && code == BN_CLICKED) {
                UpdateOrientation(data, PrintOrientationMode::Portrait);
                return TRUE;
            }
            if (id == IDC_PP_ORIENT_LANDSCAPE && code == BN_CLICKED) {
                UpdateOrientation(data, PrintOrientationMode::Landscape);
                return TRUE;
            }
            if (id == IDC_PP_DUPLEX && code == BN_CLICKED) {
                data->state.duplex = IsDlgButtonChecked(hwnd, IDC_PP_DUPLEX) == BST_CHECKED;
                return TRUE;
            }
            if (id == IDC_PP_PAPER_SOURCE_BY_SIZE && code == BN_CLICKED) {
                data->state.advanced.paperSourceByPageSize =
                    IsDlgButtonChecked(hwnd, IDC_PP_PAPER_SOURCE_BY_SIZE) == BST_CHECKED;
                return TRUE;
            }
            if (id == IDC_PP_PREV && code == BN_CLICKED && data->state.previewPage > 1) {
                data->state.previewPage--;
                InvalidatePreviewState(data, PreviewInvalidation::Page);
                return TRUE;
            }
            if (id == IDC_PP_NEXT && code == BN_CLICKED && data->state.previewPage < data->state.pageCount) {
                data->state.previewPage++;
                InvalidatePreviewState(data, PreviewInvalidation::Page);
                return TRUE;
            }
            if (id == IDC_PP_SYSTEM_PRINT && code == BN_CLICKED) {
                EndDialog(hwnd, IDC_PP_SYSTEM_PRINT);
                return TRUE;
            }
            if (id == IDC_PP_PRINT && code == BN_CLICKED) {
                if (data->state.layout.scale == PrintScaleMode::Custom && !ReadCustomScale(data)) {
                    return TRUE;
                }
                SetRangesForMode(data);
                if (!data->session || !data->rangeValid || len(data->state.ranges) == 0) {
                    MessageBoxWarning(hwnd, _TRA("Enter a valid page range"), _TRA("Printing problem."));
                    return TRUE;
                }
                EndDialog(hwnd, IDC_PP_PRINT);
                return TRUE;
            }
            if (id == IDCANCEL && code == BN_CLICKED) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;

        case WM_DESTROY:
            if (data) {
                data->closing = true;
                delete data->renderer;
                data->renderer = nullptr;
                delete data->session;
                data->session = nullptr;
            }
            return TRUE;
    }
    return FALSE;
}

PrintDialogAction ShowPrintPreviewDialog(MainWindow* win, EngineBase* engine, int currentPage,
                                         PrintScaleAdv defaultScale, const DEVMODEW* defaultDevMode,
                                         PrintDialogOutput& output) {
    output.printer = nullptr;
    VecClear(output.ranges);
    output.advanced = Print_Advanced_Data(PrintRangeAdv::All, defaultScale);
    output.layout = PrintLayoutOptionsFromAdvanced(output.advanced);

    PrintPreviewDialogData data;
    data.win = win;
    data.engine = engine;
    data.state.pageCount = engine ? engine->PageCount() : 0;
    data.state.currentDocumentPage = std::max(1, std::min(currentPage, data.state.pageCount));
    data.state.previewPage = data.state.currentDocumentPage;
    data.state.advanced = output.advanced;
    data.state.layout = output.layout;
    if (data.state.layout.scale == PrintScaleMode::LegacyNone) {
        data.state.layout.scale = PrintScaleMode::Actual;
    }

    TempStr defaultName = GetDefaultPrinterNameTemp();
    const DEVMODEW* remembered = defaultDevMode;
    if (remembered && !str::EqI(ToUtf8Temp(remembered->dmDeviceName), defaultName)) {
        remembered = nullptr;
    }
    data.session = NewPrinterSession(defaultName, remembered);
    if (!data.session) {
        StrVec names;
        GetPrinterNames(names);
        if (len(names) > 0) {
            data.session = NewPrinterSession(names[0]);
        }
    }
    if (!data.session || data.state.pageCount < 1) {
        delete data.session;
        data.session = nullptr;
        return PrintDialogAction::System;
    }
    ReadDuplexState(&data);

    data.renderer = PrintPreviewRenderer::Create(engine, MkFunc0<PrintPreviewDialogData>(OnPreviewReady, &data));
    if (!data.renderer) {
        delete data.session;
        data.session = nullptr;
        return PrintDialogAction::System;
    }

    INT_PTR result =
        CreateAppDialog(IDD_PRINT_PREVIEW, win ? win->hwndFrame : nullptr, PrintPreviewProc, (LPARAM)&data);
    if (result == -1) {
        delete data.renderer;
        data.renderer = nullptr;
        delete data.session;
        data.session = nullptr;
    }
    if (result == IDC_PP_PRINT) {
        CopyPrintOutput(&data, output);
        return PrintDialogAction::Print;
    }
    if (result == IDC_PP_SYSTEM_PRINT) {
        return PrintDialogAction::System;
    }
    if (result == -1) {
        return PrintDialogAction::System;
    }
    return PrintDialogAction::Cancel;
}
