/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "PrintPreviewModel.h"

struct Printer;
enum class PrintOrientationMode;

struct PrinterSession {
    Printer* printer = nullptr;
    PrinterMetrics metrics;
    u32 generation = 1;

    ~PrinterSession();
};

enum class PrinterPropertyResult {
    Applied,
    Cancelled,
    Failed
};

void GetPrinterNames(StrVec& names);
PrinterSession* NewPrinterSession(Str printerName);
PrinterSession* NewPrinterSession(Str printerName, const DEVMODEW* devMode);
PrinterSession* ClonePrinterSession(const PrinterSession* session);
PrinterPropertyResult ShowPrinterProperties(HWND owner, PrinterSession*& session);
bool SetPrinterPaper(PrinterSession*& session, WORD paperId);
bool SetPrinterOrientation(PrinterSession*& session, PrintOrientationMode orientation);
bool SetPrinterBin(PrinterSession*& session, WORD binId);
