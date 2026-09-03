/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "SumatraDialogs.h"
#include "Print.h"
#include "PrintLayout.h"
#include "PrintPreviewPrinter.h"

static DEVMODEW* CloneDevMode(const DEVMODEW* source) {
    if (!source || source->dmSize < sizeof(DEVMODEW)) {
        return nullptr;
    }
    size_t size = (size_t)source->dmSize + source->dmDriverExtra;
    return (DEVMODEW*)MemDup(nullptr, source, size);
}

static bool ReadPrinterMetrics(Printer* printer, PrinterMetrics& metrics) {
    if (!printer || !printer->devMode || !printer->name) {
        return false;
    }

    WCHAR* name = CWStrTemp(printer->name);
    AutoDeleteDC hdc{CreateDCW(nullptr, name, nullptr, printer->devMode)};
    if (!hdc) {
        return false;
    }

    Size paperSize(GetDeviceCaps(hdc, PHYSICALWIDTH), GetDeviceCaps(hdc, PHYSICALHEIGHT));
    Rect printable(GetDeviceCaps(hdc, PHYSICALOFFSETX), GetDeviceCaps(hdc, PHYSICALOFFSETY),
                   GetDeviceCaps(hdc, HORZRES), GetDeviceCaps(hdc, VERTRES));
    float dpiX = (float)GetDeviceCaps(hdc, LOGPIXELSX);
    float dpiY = (float)GetDeviceCaps(hdc, LOGPIXELSY);
    bool portrait = paperSize.dx <= paperSize.dy;
    if (printer->devMode->dmFields & DM_ORIENTATION) {
        portrait = printer->devMode->dmOrientation == DMORIENT_PORTRAIT;
    }
    WORD paperId = (printer->devMode->dmFields & DM_PAPERSIZE) ? printer->devMode->dmPaperSize : 0;
    return BuildPrinterMetrics(paperSize, printable, dpiX, dpiY, paperId, portrait, printer->isDuplex, metrics);
}

static PrinterSession* BuildSession(Str printerName, const DEVMODEW* devMode) {
    Printer* printer = NewPrinter(printerName);
    if (!printer) {
        return nullptr;
    }
    if (devMode) {
        DEVMODEW* copy = CloneDevMode(devMode);
        if (!copy) {
            delete printer;
            return nullptr;
        }
        printer->SetDevMode(copy);
    }

    auto* session = new PrinterSession();
    session->printer = printer;
    if (!ReadPrinterMetrics(printer, session->metrics)) {
        delete session;
        return nullptr;
    }
    return session;
}

static bool NormalizeDevMode(Str printerName, DEVMODEW* devMode) {
    if (!devMode) {
        return false;
    }

    HANDLE hPrinter = nullptr;
    WCHAR* name = CWStrTemp(printerName);
    if (!OpenPrinterW(name, &hPrinter, nullptr)) {
        return false;
    }
    LONG result = DocumentPropertiesW(nullptr, hPrinter, name, devMode, devMode, DM_IN_BUFFER | DM_OUT_BUFFER);
    ClosePrinter(hPrinter);
    return result == IDOK;
}

static bool ReplaceSession(PrinterSession*& session, DEVMODEW* devMode) {
    if (!session || !session->printer || !devMode) {
        free(devMode);
        return false;
    }
    if (!NormalizeDevMode(session->printer->name, devMode)) {
        free(devMode);
        return false;
    }

    PrinterSession* candidate = BuildSession(session->printer->name, devMode);
    free(devMode);
    if (!candidate) {
        return false;
    }
    candidate->generation = session->generation + 1;
    delete session;
    session = candidate;
    return true;
}

PrinterSession::~PrinterSession() {
    delete printer;
    printer = nullptr;
}

void GetPrinterNames(StrVec& names) {
    names.Reset();

    DWORD bytes = 0;
    DWORD count = 0;
    DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, nullptr, 2, nullptr, 0, &bytes, &count);
    if (bytes == 0) {
        return;
    }

    auto* info = (PRINTER_INFO_2*)calloc(bytes, 1);
    if (!info) {
        return;
    }
    bool ok = EnumPrintersW(flags, nullptr, 2, (LPBYTE)info, bytes, &bytes, &count) != 0;
    if (ok) {
        for (DWORD i = 0; i < count; i++) {
            if (info[i].pPrinterName) {
                names.Append(ToUtf8Temp(info[i].pPrinterName));
            }
        }
    }
    free(info);
}

PrinterSession* NewPrinterSession(Str printerName) {
    return NewPrinterSession(printerName, nullptr);
}

PrinterSession* NewPrinterSession(Str printerName, const DEVMODEW* devMode) {
    if (!printerName) {
        return nullptr;
    }
    return BuildSession(printerName, devMode);
}

PrinterSession* ClonePrinterSession(const PrinterSession* session) {
    if (!session || !session->printer) {
        return nullptr;
    }
    PrinterSession* copy = BuildSession(session->printer->name, session->printer->devMode);
    if (copy) {
        copy->generation = session->generation;
    }
    return copy;
}

PrinterPropertyResult ShowPrinterProperties(HWND owner, PrinterSession*& session) {
    if (!session || !session->printer || !session->printer->devMode) {
        return PrinterPropertyResult::Failed;
    }

    DEVMODEW* working = CloneDevMode(session->printer->devMode);
    if (!working) {
        return PrinterPropertyResult::Failed;
    }

    HANDLE hPrinter = nullptr;
    WCHAR* name = CWStrTemp(session->printer->name);
    if (!OpenPrinterW(name, &hPrinter, nullptr)) {
        free(working);
        return PrinterPropertyResult::Failed;
    }
    LONG result =
        DocumentPropertiesW(owner, hPrinter, name, working, working, DM_IN_BUFFER | DM_OUT_BUFFER | DM_IN_PROMPT);
    ClosePrinter(hPrinter);
    if (result == IDCANCEL) {
        free(working);
        return PrinterPropertyResult::Cancelled;
    }
    if (result != IDOK) {
        free(working);
        return PrinterPropertyResult::Failed;
    }
    if (!ReplaceSession(session, working)) {
        return PrinterPropertyResult::Failed;
    }
    return PrinterPropertyResult::Applied;
}

bool SetPrinterPaper(PrinterSession*& session, WORD paperId) {
    if (!session || !session->printer || paperId == 0) {
        return false;
    }
    DEVMODEW* working = CloneDevMode(session->printer->devMode);
    if (!working) {
        return false;
    }
    working->dmPaperSize = (short)paperId;
    working->dmFields |= DM_PAPERSIZE;
    return ReplaceSession(session, working);
}

bool SetPrinterOrientation(PrinterSession*& session, PrintOrientationMode orientation) {
    if (!session || !session->printer) {
        return false;
    }
    if (orientation == PrintOrientationMode::Auto) {
        DEVMODEW* working = CloneDevMode(session->printer->devMode);
        if (!working) {
            return false;
        }
        working->dmFields &= ~DM_ORIENTATION;
        return ReplaceSession(session, working);
    }

    DEVMODEW* working = CloneDevMode(session->printer->devMode);
    if (!working) {
        return false;
    }
    working->dmOrientation = orientation == PrintOrientationMode::Portrait ? DMORIENT_PORTRAIT : DMORIENT_LANDSCAPE;
    working->dmFields |= DM_ORIENTATION;
    return ReplaceSession(session, working);
}

bool SetPrinterBin(PrinterSession*& session, WORD binId) {
    if (!session || !session->printer || binId == 0) {
        return false;
    }
    DEVMODEW* working = CloneDevMode(session->printer->devMode);
    if (!working) {
        return false;
    }
    working->dmDefaultSource = (short)binId;
    working->dmFields |= DM_DEFAULTSOURCE;
    return ReplaceSession(session, working);
}
