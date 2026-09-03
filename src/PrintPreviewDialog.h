/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct MainWindow;
struct Printer;

enum class PrintDialogAction {
    Cancel,
    Print,
    System
};

struct PrintDialogOutput {
    Printer* printer = nullptr;
    Vec<PRINTPAGERANGE> ranges;
    PrintLayoutOptions layout;
    Print_Advanced_Data advanced;
};

PrintDialogAction ShowPrintPreviewDialog(MainWindow* win, EngineBase* engine, int currentPage,
                                         PrintScaleAdv defaultScale, const DEVMODEW* defaultDevMode,
                                         PrintDialogOutput& output);
