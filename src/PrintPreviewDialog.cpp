/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "gui/UIModels.h"
#include "gui/Gfx.h"

#include "Settings.h"
#include "AppSettings.h"
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
    PrintDialogOutput* output = nullptr;
    PrintPreviewRenderer* renderer = nullptr;
    PrintDialogState state;
    bool rangeValid = true;
    bool closing = false;
    HWND previewGroup = nullptr;
    int previewGroupRightMargin = 0;
    int previewLeftInset = 0;
    int previewRightInset = 0;
    bool previewGeometryReady = false;
};

static PrintPreviewDialogData* DialogData(HWND hwnd) {
    return (PrintPreviewDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static bool IsPrintPreviewKoreanUi() {
    Str lang = trans::GetCurrentLangCode();
    if (str::Eq(lang, StrL("kr")) || str::Eq(lang, StrL("ko"))) {
        return true;
    }
    return gSettings && (str::Eq(gSettings->uiLanguage, StrL("kr")) || str::Eq(gSettings->uiLanguage, StrL("ko")));
}

static Str PrintUiText(Str translated, Str korean) {
    return IsPrintPreviewKoreanUi() ? korean : translated;
}

static void LocalizeChildByCaption(HWND hwnd, Str english, Str localized) {
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (str::Eq(HwndGetTextTemp(child), english)) {
            HwndSetText(child, localized);
        }
    }
}

static void LocalizePrintPreviewChrome(HWND hwnd) {
    HwndSetText(hwnd, PrintUiText(_TRA("Print"), StrL("인쇄")));
    LocalizeChildByCaption(hwnd, StrL("Printer:"), PrintUiText(_TRA("Printer:"), StrL("프린터:")));
    LocalizeChildByCaption(hwnd, StrL("Copies:"), PrintUiText(_TRA("Copies:"), StrL("매수:")));
    LocalizeChildByCaption(hwnd, StrL("Pages"), PrintUiText(_TRA("Pages"), StrL("페이지")));
    LocalizeChildByCaption(hwnd, StrL("Scaling"), PrintUiText(_TRA("Scaling"), StrL("배율")));
    LocalizeChildByCaption(hwnd, StrL("Orientation"), PrintUiText(_TRA("Orientation"), StrL("방향")));
    LocalizeChildByCaption(hwnd, StrL("Preview"), PrintUiText(_TRA("Preview"), StrL("미리 보기")));
    LocalizeChildByCaption(hwnd, StrL("Information"), PrintUiText(_TRA("Information"), StrL("정보")));
    LocalizeChildByCaption(hwnd, StrL("Document:"), PrintUiText(_TRA("Document:"), StrL("문서:")));
    LocalizeChildByCaption(hwnd, StrL("Paper:"), PrintUiText(_TRA("Paper:"), StrL("용지:")));
    LocalizeChildByCaption(hwnd, StrL("Printable:"), PrintUiText(_TRA("Printable:"), StrL("인쇄 영역:")));
}

static void LocalizePageSetupChrome(HWND hwnd) {
    HwndSetText(hwnd, PrintUiText(_TRA("Page Setup"), StrL("페이지 설정")));
    LocalizeChildByCaption(hwnd, StrL("Paper:"), PrintUiText(_TRA("Paper:"), StrL("용지:")));
    LocalizeChildByCaption(hwnd, StrL("Source:"), PrintUiText(_TRA("Source:"), StrL("용지 공급:")));
    LocalizeChildByCaption(hwnd, StrL("Orientation"), PrintUiText(_TRA("Orientation"), StrL("방향")));
}

static void LocalizeAdvancedChrome(HWND hwnd) {
    HwndSetText(hwnd, PrintUiText(_TRA("Advanced"), StrL("고급 설정")));
    LocalizeChildByCaption(hwnd, StrL("Print range"), PrintUiText(_TRA("Print range"), StrL("인쇄 범위")));
    LocalizeChildByCaption(hwnd, StrL("Rotate printout:"), PrintUiText(_TRA("Rotate printout:"), StrL("인쇄물 회전:")));
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
                       fmt(PrintUiText(_TRA("Page %d of %d"), StrL("페이지 %d / %d")).s, data->state.previewPage,
                           data->state.pageCount));
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
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAPER_SIZE, fmt("%.1f x %.1f mm", metrics.paperMm.dx, metrics.paperMm.dy));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINTABLE_SIZE,
                       fmt("%.1f x %.1f mm", metrics.printableMm.dx, metrics.printableMm.dy));
    HwndSetDlgItemText(data->hwnd, IDC_PP_DOC_SIZE,
                       fmt("%.0f x %.0f px", data->engine->PageMediabox(data->state.previewPage).dx,
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
    HwndSetDlgItemText(data->hwnd, IDC_PP_EFFECTIVE_SCALE,
                       fmt(PrintUiText(_TRA("Scale: %.1f%%"), StrL("배율: %.1f%%")).s, scale));
    if (clipping.pageBoundsOutsidePrintable || clipping.contentOutsidePrintable) {
        HwndSetDlgItemText(data->hwnd, IDC_PP_CLIPPING,
                           fmt(PrintUiText(_TRA("Clipping: L %.1f  R %.1f  T %.1f  B %.1f mm"),
                                           StrL("잘림: 좌 %.1f  우 %.1f  상 %.1f  하 %.1f mm"))
                                   .s,
                               clipping.leftMm, clipping.rightMm, clipping.topMm, clipping.bottomMm));
    } else {
        HwndSetDlgItemText(data->hwnd, IDC_PP_CLIPPING, PrintUiText(_TRA("Clipping: none"), StrL("잘림: 없음")));
    }
    HwndSetDlgItemText(
        data->hwnd, IDC_PP_OUTPUT_SIZE,
        fmt(PrintUiText(_TRA("Output: %d x %d px"), StrL("출력: %d x %d px")).s, layout.target.dx, layout.target.dy));
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
            LocalizePageSetupChrome(hwnd);
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_PAPER, PrintUiText(_TRA("Paper"), StrL("용지")));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_BIN, PrintUiText(_TRA("Paper source"), StrL("용지 공급원")));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_PORTRAIT, PrintUiText(_TRA("Portrait"), StrL("세로")));
            HwndSetDlgItemText(hwnd, IDC_PP_SETUP_LANDSCAPE, PrintUiText(_TRA("Landscape"), StrL("가로")));
            HwndSetDlgItemText(hwnd, IDOK, PrintUiText(_TRA("OK"), StrL("확인")));
            HwndSetDlgItemText(hwnd, IDCANCEL, PrintUiText(_TRA("Cancel"), StrL("취소")));
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
            LocalizeAdvancedChrome(hwnd);
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_ALL, PrintUiText(_TRA("All pages"), StrL("모든 페이지")));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_EVEN,
                               PrintUiText(_TRA("Even pages only"), StrL("짝수 페이지만")));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_RANGE_ODD, PrintUiText(_TRA("Odd pages only"), StrL("홀수 페이지만")));
            HwndSetDlgItemText(
                hwnd, IDC_PP_ADV_PER_PAGE,
                PrintUiText(_TRA("Use each page's paper size"), StrL("각 페이지의 문서 용지 크기 사용")));
            HwndSetDlgItemText(hwnd, IDC_PP_ADV_ROTATE, PrintUiText(_TRA("Rotation"), StrL("회전")));
            HwndSetDlgItemText(hwnd, IDOK, PrintUiText(_TRA("OK"), StrL("확인")));
            HwndSetDlgItemText(hwnd, IDCANCEL, PrintUiText(_TRA("Cancel"), StrL("취소")));
            HWND rotate = GetDlgItem(hwnd, IDC_PP_ADV_ROTATE);
            CbAddString(rotate, PrintUiText(_TRA("None"), StrL("없음")));
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
        MessageBoxWarning(data->hwnd,
                          PrintUiText(_TRA("Couldn't initialize printer"), StrL("프린터를 초기화할 수 없습니다.")),
                          PrintUiText(_TRA("Printing problem."), StrL("인쇄 오류")));
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

static constexpr int kMinPreviewWidth = 120;

static HWND FindChildByCaption(HWND hwnd, Str caption) {
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (str::Eq(HwndGetTextTemp(child), caption)) {
            return child;
        }
    }
    return nullptr;
}

static void RememberPreviewGeometry(PrintPreviewDialogData* data) {
    data->previewGroup = FindChildByCaption(data->hwnd, StrL("Preview"));
    HWND preview = GetDlgItem(data->hwnd, IDC_PP_PREVIEW);
    if (!data->previewGroup || !preview) {
        return;
    }

    RECT group;
    RECT previewRect;
    RECT client;
    GetWindowRect(data->previewGroup, &group);
    GetWindowRect(preview, &previewRect);
    GetClientRect(data->hwnd, &client);
    MapWindowPoints(nullptr, data->hwnd, (POINT*)&group, 2);
    MapWindowPoints(nullptr, data->hwnd, (POINT*)&previewRect, 2);
    data->previewGroupRightMargin = std::max(0, (int)client.right - (int)group.right);
    data->previewLeftInset = (int)previewRect.left - (int)group.left;
    data->previewRightInset = (int)group.right - (int)previewRect.right;
    data->previewGeometryReady = true;
}

static void ResizePreview(PrintPreviewDialogData* data) {
    if (!data || !data->previewGeometryReady || !data->previewGroup) {
        return;
    }

    HWND preview = GetDlgItem(data->hwnd, IDC_PP_PREVIEW);
    RECT group;
    RECT previewRect;
    RECT client;
    GetWindowRect(data->previewGroup, &group);
    GetWindowRect(preview, &previewRect);
    GetClientRect(data->hwnd, &client);
    MapWindowPoints(nullptr, data->hwnd, (POINT*)&group, 2);
    MapWindowPoints(nullptr, data->hwnd, (POINT*)&previewRect, 2);

    int groupHeight = (int)group.bottom - (int)group.top;
    int groupWidth = std::max(kMinPreviewWidth + data->previewLeftInset + data->previewRightInset,
                              (int)client.right - data->previewGroupRightMargin - (int)group.left);
    MoveWindow(data->previewGroup, group.left, group.top, groupWidth, groupHeight, TRUE);

    int previewWidth = std::max(kMinPreviewWidth, groupWidth - data->previewLeftInset - data->previewRightInset);
    MoveWindow(preview, group.left + data->previewLeftInset, previewRect.top, previewWidth,
               (int)previewRect.bottom - (int)previewRect.top, TRUE);
}

static void SetInitialControls(PrintPreviewDialogData* data) {
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINTER, PrintUiText(_TRA("Printer"), StrL("프린터")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PROPERTIES, PrintUiText(_TRA("Properties..."), StrL("속성...")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ADVANCED, PrintUiText(_TRA("Advanced..."), StrL("고급 설정...")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_COPIES, PrintUiText(_TRA("Copies:"), StrL("매수:")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_ALL, PrintUiText(_TRA("All"), StrL("전체")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_CURRENT, PrintUiText(_TRA("Current page"), StrL("현재 페이지")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_RANGE, PrintUiText(_TRA("Pages:"), StrL("페이지:")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_FIT, PrintUiText(_TRA("Fit"), StrL("맞춤")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_ACTUAL, PrintUiText(_TRA("Actual size"), StrL("실제 크기")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_SHRINK, PrintUiText(_TRA("Shrink"), StrL("축소")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SCALE_CUSTOM, PrintUiText(_TRA("Custom:"), StrL("사용자 지정:")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAPER_SOURCE_BY_SIZE,
                       PrintUiText(_TRA("Choose paper source by size"), StrL("페이지 크기에 따라 용지 공급원 선택")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_DUPLEX, PrintUiText(_TRA("Duplex"), StrL("양면 인쇄")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_AUTO, PrintUiText(_TRA("Auto"), StrL("자동")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_PORTRAIT, PrintUiText(_TRA("Portrait"), StrL("세로")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_ORIENT_LANDSCAPE, PrintUiText(_TRA("Landscape"), StrL("가로")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PREV, PrintUiText(_TRA("Previous"), StrL("이전")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_NEXT, PrintUiText(_TRA("Next"), StrL("다음")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PAGE_SETUP, PrintUiText(_TRA("Page setup..."), StrL("페이지 설정...")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_SYSTEM_PRINT, PrintUiText(_TRA("System print..."), StrL("시스템 인쇄...")));
    HwndSetDlgItemText(data->hwnd, IDC_PP_PRINT, PrintUiText(_TRA("Print"), StrL("인쇄")));
    HwndSetDlgItemText(data->hwnd, IDCANCEL, PrintUiText(_TRA("Cancel"), StrL("취소")));
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
            RememberPreviewGeometry(data);
            ResizePreview(data);
            LocalizePrintPreviewChrome(hwnd);
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
                ResizePreview(data);
                InvalidateRect(GetDlgItem(hwnd, IDC_PP_PREVIEW), nullptr, FALSE);
            }
            return TRUE;

        case WM_GETMINMAXINFO:
            if (data && data->previewGeometryReady) {
                auto* minMax = (MINMAXINFO*)lp;
                RECT client;
                RECT window;
                RECT group;
                RECT copies;
                GetClientRect(hwnd, &client);
                GetWindowRect(hwnd, &window);
                GetWindowRect(data->previewGroup, &group);
                GetWindowRect(GetDlgItem(hwnd, IDC_PP_COPIES), &copies);
                MapWindowPoints(nullptr, hwnd, (POINT*)&group, 2);
                MapWindowPoints(nullptr, hwnd, (POINT*)&copies, 2);

                int minPreviewPanel = data->previewLeftInset + kMinPreviewWidth + data->previewRightInset;
                int minClientWidth = std::max((int)group.left + minPreviewPanel + data->previewGroupRightMargin,
                                              (int)copies.right + data->previewGroupRightMargin);
                int nonClientWidth = (int)(window.right - window.left) - (int)client.right;
                int nonClientHeight = (int)(window.bottom - window.top) - (int)client.bottom;
                minMax->ptMinTrackSize.x = std::max(minMax->ptMinTrackSize.x, (LONG)(minClientWidth + nonClientWidth));
                minMax->ptMinTrackSize.y =
                    std::max(minMax->ptMinTrackSize.y, (LONG)((int)client.bottom + nonClientHeight));
                return TRUE;
            }
            return FALSE;
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
                    MessageBoxWarning(
                        hwnd, PrintUiText(_TRA("Enter a valid page range"), StrL("올바른 페이지 범위를 입력하십시오.")),
                        PrintUiText(_TRA("Printing problem."), StrL("인쇄 오류")));
                    return TRUE;
                }
                CopyPrintOutput(data, *data->output);
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
    data.output = &output;
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
