# SumatraPDF Custom Print Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace interactive `Ctrl+P` for fixed-page documents with a Sumatra-owned Acrobat-style print dialog that shows a live preview and preserves exact parity with actual printer layout.

**Architecture:** Keep `Print.cpp` as the print engine. Extract reusable print-layout math into a pure module, add a transactional printer-session adapter, reuse `PageRenderService` for asynchronous print-target rasterization, and implement a resource-backed Win32 dialog with an owner-drawn preview surface. The existing Win11/PrintDlgEx path stays available as an explicit fallback and remains the path for command-line, synchronous, selection, CHM, and Markdown printing.

**Tech Stack:** C++/Win32, SumatraPDF base containers/strings, GDI printer HDC, existing `Gfx` Direct2D/GDI+ abstraction, existing `PageRenderService`, Bun TypeScript GUI tests.

**Spec:** `docs/superpowers/specs/2026-09-02-custom-print-preview-design.md`

## Global Constraints

- Preserve the repository rules in `agents.md`: no STL introduction, existing include order, no `#pragma once`, clang-format touched C/C++ before build, `bun cmd/format.ts -ts` for touched TypeScript.
- Never change command-line printing semantics. `PrintFile()` / `PrintFile2()` and `-print-to` stay on the existing synchronous path.
- Do not route selection printing through the custom dialog in v1. If `selectionOnPage` exists, keep the existing system print flow.
- Preview and actual print must use the same layout result. Do not implement preview-only Fit, rotation, centering, Actual Size, or Custom Scale formulas.
- Printer device pixels are canonical layout coordinates. Convert to millimetres only for display and diagnostics.
- Treat X/Y DPI independently.
- Keep manufacturer-only features inside `DocumentPropertiesW()`.
- Do not remove `PrintWin11.cpp` in v1.
- Implementation commits are explicit checkpoints. Repository policy says not to commit implementation work without an explicit user command.

---

## Completion Record — 2026-09-03

Custom Print Preview v1 implementation is complete through the implemented
feature scope covered by this plan.

Current verification:

- Debug build: PASS, 0 warnings / 0 errors
- Unit tests: PASS
- Custom Print Preview regression: PASS
- Print-button shutdown regression: PASS
- One-page Microsoft Print to PDF output: PASS, verified as one page

Final physical acceptance remains CONDITIONAL PASS pending human QA:

- Physical ApeosPort output compared against Preview
- Native/System Print open, cancel, and re-entry verification

No acceptance-blocking source defect is currently known.
No source change was required during final verification.
No additional commit was created.

---

## Task 1: Extract a testable shared print-layout core and add Actual/Custom scale

**Files:**
- Create: `src/PrintLayout.h`
- Create: `src/PrintLayout.cpp`
- Modify: `src/Print.h`
- Modify: `src/Print.cpp`
- Modify: `src/PrintWin11.cpp`
- Modify: `premake5.files.lua`
- Modify: `src/SumatraUnitTests.cpp`

### Contract

Add a new custom-dialog layout scale independent of legacy `PrintScaleAdv`:

```cpp
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
    PrintRotationAdv rotation = PrintRotationAdv::Auto;
    bool autoRotate = true;
    bool centerHorizontally = false;
    int extraRotation = 0;
};
```

Keep `Print_Advanced_Data` unchanged for the legacy system dialog. Add:

```cpp
PrintLayoutOptions PrintLayoutOptionsFromAdvanced(const Print_Advanced_Data& advanced);
```

Add a pure placement input and result so the numeric math can be unit-tested without `EngineBase`:

```cpp
struct PrintPlacementInput {
    SizeF pageSize;
    RectF contentBox;
    Size paperSize;
    Rect printable;
    float dpiX = 96.f;
    float dpiY = 96.f;
    float fileDpi = 96.f;
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
```

`PrintLayout.cpp` owns the placement math. `Print.cpp` keeps the `EngineBase` adapter that resolves rotation, `PageMediabox()`, `PageContentBox()`, and transformed content before calling the pure core.

Scale semantics:

- `LegacyNone`: preserve current Sumatra behavior exactly.
- `Shrink`, `Fit`, `Stretch`: preserve current behavior exactly.
- `Actual`: `zoom = dpiFactor`; center the physical page on the physical sheet in both axes; do not nudge content to hide clipping.
- `Custom`: `zoom = dpiFactor * customScalePercent / 100`; center the physical page on the physical sheet; do not alter the requested scale to avoid clipping.
- `physicalScale = zoom / dpiFactor` for uniform modes. For Stretch, set `physicalScale` to `0.f` and show a non-percent label in UI.
- `target` is always physical-paper coordinates: `printable + offset` for non-stretch, `printable` for stretch.

### TDD steps

- [ ] Add `PrintLayout_UnitTests()` in `PrintLayout.cpp` under `#if IS_DEBUG` before production changes.
- [ ] Add it to `SumatraPDF_UnitTests()` and add `PrintLayout.*` to `test_util_files()`.
- [ ] Write failing cases for Actual 100%, Custom 50%, Custom 150%, Fit, Shrink, Stretch, legacy None, non-square 600x1200 DPI, and invalid DPI fallback.
- [ ] Run `bun cmd/run-unit-tests.ts -dbg` and confirm the new assertions fail for missing behavior.
- [ ] Implement `PrintLayout.cpp` and make the existing `CalculatePrintPageLayout()` delegate to it.
- [ ] Add an overload that takes `PrintLayoutOptions`; keep the existing `Print_Advanced_Data` signature as a compatibility adapter.
- [ ] Update `PrintWin11.cpp` to use `layout.target` rather than re-deriving destination coordinates.
- [ ] Update normal-page GDI printing to use the same `PrintPageLayout` values; selection printing remains on legacy scale semantics.
- [ ] Run clang-format on touched C/C++ files.
- [ ] Run `bun cmd/run-unit-tests.ts -dbg` and `bun cmd/build.ts -debug`.

**Commit checkpoint:** stop, show diff/test evidence, and wait for explicit commit approval.

Suggested subject: `Add shared print layout core`

---

## Task 2: Add pure dialog-state, range parsing, printer metrics, and clipping diagnostics

**Files:**
- Create: `src/PrintPreviewModel.h`
- Create: `src/PrintPreviewModel.cpp`
- Modify: `premake5.files.lua`
- Modify: `src/SumatraUnitTests.cpp`

### Contract

Add:

```cpp
enum class PrintPageMode { All, Current, Range };
enum class PrintOrientationMode { Auto, Portrait, Landscape };
enum class PreviewInvalidation { None, Page, Printer };

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
```

Pure helpers:

```cpp
bool ParsePrintRanges(Str text, int pageCount, Vec<PRINTPAGERANGE>& ranges);
bool BuildPrinterMetrics(Size paperPx, Rect printablePx, float dpiX, float dpiY,
                         WORD paperId, bool portrait, bool duplexSupported,
                         PrinterMetrics& out);
ClippingReport CalculatePrintClipping(RectF pageTarget, RectF contentTarget,
                                      Rect printable, float dpiX, float dpiY);
void InvalidatePreview(PrintDialogState& state, PreviewInvalidation level);
```

Range syntax for v1: comma-separated `N` and `N-M`, ascending ranges only, pages limited to `1..pageCount`, whitespace allowed. Invalid input disables Print rather than silently rewriting it.

### TDD steps

- [ ] Add `PrintPreviewModel_UnitTests()` first and register it in `SumatraPDF_UnitTests()`.
- [ ] Cover `1`, `1-3`, `1-3,5`, whitespace, page 0, page > count, reversed ranges, malformed separators, and empty input.
- [ ] Cover `PrinterMetrics` with 300x300 and 600x1200 DPI and assert mm conversion within 0.1 mm.
- [ ] Cover page-bounds-only clipping, content clipping, and left/right/top/bottom millimetre values using 254 DPI so 10 px = 1 mm.
- [ ] Run unit tests and confirm new tests fail before implementation.
- [ ] Implement the model and rerun the unit tests.
- [ ] Add `PrintPreviewModel.*` to app and test-util source lists.
- [ ] Run clang-format and `bun cmd/run-unit-tests.ts -dbg`.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Add print preview state model`

---

## Task 3: Reuse PageRenderService for print-target asynchronous previews

**Files:**
- Modify: `src/PageRenderService.h`
- Modify: `src/PageRenderService.cpp`
- Create: `src/PrintPreviewRenderer.h`
- Create: `src/PrintPreviewRenderer.cpp`
- Modify: `premake5.files.lua`

### Contract

Do not create a second worker/cache system. Extend the existing service with a print factory:

```cpp
static PageRenderService* CreateForPrint(EngineBase* engine, const Func0& onPageReady,
                                         i64 maxBytes = 128 * 1024 * 1024);
```

Implementation detail: keep `RenderTarget` inside `PageRenderService.cpp`; existing `Create()` uses `RenderTarget::View`, `CreateForPrint()` uses `RenderTarget::Print`. The worker uses the stored target. Existing behavior and tests remain unchanged.

Add a thin preview renderer that:

- owns `PageRenderService::CreateForPrint()`;
- calls `NewGeneration()` for Level 1/2 invalidation;
- requests current page as `Visible`, N-1/N+1 as `Nearby`;
- derives raster zoom from `layout.zoom * paperToViewScale`, with modest oversampling and a hard equivalent cap of 192 DPI;
- draws cached `Pixmap` through `Gfx::DrawPixmap()`;
- draws paper, printable-area boundary, page bitmap, and clipping indication using existing `Gfx` APIs;
- never uses printer DPI directly as preview raster DPI.

Use an owner-drawn `STATIC` later, so the renderer accepts the paint HDC/Gfx and viewport rectangle rather than owning a window class.

### Steps

- [ ] Add a debug test to existing `PageRenderPolicy_UnitTests()` or a narrow service test proving `NewGeneration()` rejects stale work; do not duplicate already-covered LRU/priority assertions.
- [ ] Implement `CreateForPrint()` without changing the public behavior of `Create()`.
- [ ] Implement `PrintPreviewRenderer` with a 128 MB cache and N±1 request policy.
- [ ] Ensure callback only invalidates the preview HWND through a UI-thread posted callback; worker code must not touch dialog controls.
- [ ] Run clang-format.
- [ ] Run `bun cmd/run-unit-tests.ts -dbg` and `bun cmd/build.ts -debug`.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Add async print preview renderer`

---

## Task 4: Add transactional printer-session and driver-property integration

**Files:**
- Create: `src/PrintPreviewPrinter.h`
- Create: `src/PrintPreviewPrinter.cpp`
- Modify: `premake5.files.lua`

### Contract

Add:

```cpp
struct PrinterSession {
    Printer* printer = nullptr;
    PrinterMetrics metrics;
    u32 generation = 1;
};

enum class PrinterPropertyResult { Applied, Cancelled, Failed };

void GetPrinterNames(StrVec& names);
PrinterSession* NewPrinterSession(Str printerName);
PrinterPropertyResult ShowPrinterProperties(HWND owner, PrinterSession*& session);
bool SetPrinterPaper(PrinterSession*& session, WORD paperId);
bool SetPrinterOrientation(PrinterSession*& session, PrintOrientationMode orientation);
bool SetPrinterBin(PrinterSession*& session, WORD binId);
```

`NewPrinterSession()`:

1. call existing `NewPrinter()`;
2. create HDC from the printer name + `DEVMODE`;
3. read `PHYSICALWIDTH`, `PHYSICALHEIGHT`, `PHYSICALOFFSETX/Y`, `HORZRES/VERTRES`, `LOGPIXELSX/Y`;
4. reject invalid geometry for the custom dialog instead of inventing a misleading preview;
5. build `PrinterMetrics`.

`ShowPrinterProperties()` must be transactional:

1. clone `dmSize + dmDriverExtra` bytes;
2. call `DocumentPropertiesW(... DM_IN_BUFFER | DM_OUT_BUFFER | DM_IN_PROMPT)` on the clone;
3. on Cancel, free clone and leave session untouched;
4. on OK, create a new candidate `Printer`/HDC/metrics;
5. replace the session only after candidate validation succeeds.

No vendor-private field interpretation.

### Steps

- [ ] Implement a small pure validation helper for raw device-cap values and add it to `PrintPreviewModel_UnitTests()` so invalid/valid geometry is automated.
- [ ] Implement printer enumeration and default-printer selection using existing Win32 helpers.
- [ ] Implement candidate-build/swap logic for Properties, paper, orientation, and bin changes.
- [ ] Preserve custom paper returned by a driver even though v1 has no custom-paper creation UI.
- [ ] Disable duplex at model/UI level when `Printer::isDuplex` is false.
- [ ] Run clang-format and `bun cmd/build.ts -debug`.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Add transactional printer session`

---

## Task 5: Add the resource-backed Acrobat-style dialog shell

**Files:**
- Create: `src/PrintPreviewDialog.h`
- Create: `src/PrintPreviewDialog.cpp`
- Modify: `src/SumatraPDF.rc`
- Modify: `src/resource.h`
- Modify: `src/SumatraDialogs.cpp`
- Modify: `src/SumatraDialogs.h`
- Modify: `premake5.files.lua`

### Resource IDs

Use the next free resource values shown by `resource.h`:

```text
IDD_PRINT_PREVIEW        144
IDD_PRINT_PAGE_SETUP     145
IDD_PRINT_ADVANCED_V1    146
```

Start control IDs at 1085. Use explicit names, grouped roughly as:

```text
IDC_PP_PRINTER
IDC_PP_PROPERTIES
IDC_PP_ADVANCED
IDC_PP_COPIES
IDC_PP_PAGE_ALL
IDC_PP_PAGE_CURRENT
IDC_PP_PAGE_RANGE
IDC_PP_PAGE_RANGE_EDIT
IDC_PP_SCALE_FIT
IDC_PP_SCALE_ACTUAL
IDC_PP_SCALE_SHRINK
IDC_PP_SCALE_CUSTOM
IDC_PP_SCALE_CUSTOM_EDIT
IDC_PP_PAPER_SOURCE_BY_SIZE
IDC_PP_DUPLEX
IDC_PP_ORIENT_AUTO
IDC_PP_ORIENT_PORTRAIT
IDC_PP_ORIENT_LANDSCAPE
IDC_PP_PREVIEW
IDC_PP_DOC_SIZE
IDC_PP_PAPER_SIZE
IDC_PP_PRINTABLE_SIZE
IDC_PP_EFFECTIVE_SCALE
IDC_PP_OUTPUT_SIZE
IDC_PP_CLIPPING
IDC_PP_PREV
IDC_PP_NEXT
IDC_PP_PAGE_INDICATOR
IDC_PP_PAGE_SETUP
IDC_PP_SYSTEM_PRINT
IDC_PP_PRINT
```

`IDC_PP_PREVIEW` is an `SS_OWNERDRAW` static.

Expose the existing RTL/font-size-aware dialog creation helper from `SumatraDialogs.cpp` under a short application-level name instead of duplicating template manipulation in the new file.

### Dialog interface

```cpp
enum class PrintDialogAction { Cancel, Print, System };

struct PrintDialogOutput {
    Printer* printer = nullptr;
    Vec<PRINTPAGERANGE> ranges;
    PrintLayoutOptions layout;
    Print_Advanced_Data advanced;
};

PrintDialogAction ShowPrintPreviewDialog(MainWindow* win, EngineBase* engine,
                                         int currentPage, PrintScaleAdv defaultScale,
                                         PrintDialogOutput& output);
```

On `Print`, ownership of `output.printer` transfers to the caller. On Cancel/System, the dialog frees its session.

### Steps

- [ ] Add the dialog resources with English strings marked through runtime translation assignments where the project requires them.
- [ ] Implement initialization: default printer, printer list, page count/current page, default scale mapping, copies=1, duplex capability.
- [ ] Implement all/current/range controls and validation.
- [ ] Implement Fit/Actual/Shrink/Custom controls; custom input accepts 1.0–1000.0%.
- [ ] Implement Auto/Portrait/Landscape controls.
- [ ] Implement Previous/Next page navigation constrained to document pages.
- [ ] Keep Print disabled if no valid printer, invalid range, or invalid custom percentage.
- [ ] `System Print...` is always a visible secondary action.
- [ ] Add a 150 ms timer for custom-scale edit debounce; do not rerender on every keystroke.
- [ ] Run clang-format and `bun cmd/build.ts -debug`.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Add custom print dialog shell`

---

## Task 6: Wire live preview, metadata, invalidation levels, and clipping UI

**Files:**
- Modify: `src/PrintPreviewDialog.cpp`
- Modify: `src/PrintPreviewRenderer.cpp`
- Modify: `src/PrintPreviewModel.cpp`

### Steps

- [ ] On dialog open, calculate layout for `previewPage` using the current `PrinterMetrics` and request its bitmap immediately.
- [ ] In `WM_DRAWITEM` for `IDC_PP_PREVIEW`, create `Gfx` from the supplied HDC and draw: background, paper shadow, paper, non-printable area, printable boundary, document bitmap, clipping overlay.
- [ ] Compute paper-to-view scale only after print layout is finished; it must never feed back into printer layout.
- [ ] Show document size, paper size/name, printable size, output size, and effective scale in the right panel.
- [ ] For Stretch, show `Stretch` instead of a fake percent.
- [ ] Show separate clipping text for page-box overflow vs real content overflow. Content overflow is the warning condition.
- [ ] Level 0 changes: copies and duplex update state only.
- [ ] Level 1 changes: scale/orientation/preview page call `NewGeneration()` and request current/N±1.
- [ ] Level 2 changes: printer, paper, Properties, Page Setup rebuild session, clear renderer generation/cache, recalculate layout, request preview.
- [ ] Ensure a late callback from an obsolete generation cannot repaint an obsolete bitmap as current state.
- [ ] Verify UI remains responsive while rendering a large page by rapidly toggling Fit/Actual and moving pages.
- [ ] Run clang-format, unit tests, and debug build.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Wire live print preview`

---

## Task 7: Add Page Setup and custom Advanced dialogs

**Files:**
- Modify: `src/PrintPreviewDialog.cpp`
- Modify: `src/SumatraPDF.rc`
- Modify: `src/resource.h`
- Modify: `src/SumatraDialogs.cpp`
- Modify: `src/SumatraDialogs.h`

### Page Setup v1

- paper size from `Printer::papers/paperNames/paperSizes`;
- paper source/bin from `Printer::bins/binNames`;
- Portrait/Landscape;
- no user margin fields.

Operate on a candidate printer session. OK commits the rebuilt session; Cancel discards it.

### Advanced v1

Do not reuse the legacy property page unchanged because its Page Scaling section would create a second scale source of truth. The custom Advanced dialog exposes only settings not already owned by the main dialog:

- all/odd/even filtering (`PrintRangeAdv`);
- per-page paper size for mixed-size PDFs;
- extra rotation 0/90/180/270.

`paperSourceByPageSize` remains on the main dialog. Custom scale remains only in `PrintLayoutOptions`.

### Steps

- [ ] Add candidate-copy behavior for Page Setup and Advanced.
- [ ] On Page Setup OK, rebuild metrics from HDC before committing.
- [ ] On Advanced OK, compare old/new values and select Level 0/1/2 invalidation appropriately; extra rotation and mixed-size paper handling are preview-affecting.
- [ ] On either Cancel, leave main-dialog state byte-for-byte unchanged for the affected structures.
- [ ] Run clang-format and debug build.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Add print setup dialogs`

---

## Task 8: Route interactive Ctrl+P through the custom dialog and preserve system fallback

**Files:**
- Modify: `src/Print.cpp`
- Modify: `src/Print.h` if helper declarations are required

### Routing rules

Refactor the current `PrintCurrentFile()` rather than duplicating the PrintDlgEx block.

Expected routing:

```text
PrintCurrentFile
  permission/document/restriction checks
  CHM/Markdown -> existing behavior
  fixed document
    waitForCompletion -> existing synchronous system/classic path
    selection present -> existing system path
    normal interactive -> custom dialog
        Print  -> existing PrintData/PrintToDevice thread
        System -> existing Win11 -> PrintDlgEx fallback chain
        Cancel -> return
```

Keep the existing Win11 system dialog as the first system fallback on Windows 11, then `PrintDlgEx` if WinRT is unavailable.

Extend `PrintData` with a `PrintLayoutOptions layoutOptions` field. Legacy/system construction uses `PrintLayoutOptionsFromAdvanced(advanced)`. Custom-dialog construction uses the custom `Actual/Custom` options. Normal-page `PrintToDevice()` calls the layout overload that takes `layoutOptions`. Selection printing continues to use legacy `Print_Advanced_Data` scaling code.

Before starting the print thread after a modal custom dialog:

1. revalidate `MainWindow` and current fixed document;
2. reacquire engine and page count;
3. validate ranges against the current page count;
4. validate the returned printer session/devmode by creating its HDC once more;
5. only then construct `PrintData`.

Apply copies, duplex, selected paper/bin, and `paperSourceByPageSize` to the final `DEVMODE` immediately before ownership transfers to `PrintData`.

### Regression steps

- [ ] Run `bun cmd/run-unit-tests.ts -dbg`.
- [ ] Run `bun cmd/build.ts -debug`.
- [ ] Run existing print-to-PDF regressions, including `bun tests/issue-4967.ts --no-build`, to prove command-line printing still bypasses the custom UI.
- [ ] Verify selection printing still opens the system dialog, not the custom one.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Route Ctrl+P to custom preview`

---

## Task 9: Add automated GUI acceptance for the custom dialog

**Files:**
- Create: `tests/print-preview-dialog.ts`
- Modify: `tests/run-almost-all.ts`
- Modify: `tests/winapi.ts` only if one missing Win32 helper is necessary

Use existing `custom-zoom-dialog.ts`, `win-automation.ts`, and `print-util.ts` patterns. Always launch with `-for-testing` through existing helpers.

### Test fixture

Use `writeMultiPagePdf()` to generate a three-page PDF. Open it in a controlled Sumatra process, send `CmdPrint`, and find the new top-level dialog in the same PID.

### Required assertions

- [ ] Dialog title is `Print` and the custom dialog, not the Windows unified print UI, opens for a normal fixed PDF.
- [ ] Printer ComboBox, Properties, Advanced, copies, page-range controls, scale controls, orientation controls, preview, navigation, Page Setup, System Print, Print, and Cancel are present.
- [ ] Initial page indicator is `1 / 3` or current-page equivalent defined by the implementation.
- [ ] Next changes to `2 / 3`; Previous returns to `1 / 3`.
- [ ] Custom Scale edit accepts `50`, selects Custom, and remains responsive after the 150 ms debounce.
- [ ] Invalid range and invalid custom scale disable Print.
- [ ] Cancel closes the dialog without starting a job.
- [ ] If a valid printer is present, wait for a non-empty preview fingerprint and confirm switching Fit -> Actual changes preview/metadata without freezing the window.
- [ ] Register the test in `run-almost-all.ts` because it must not spool a real print job.
- [ ] Run `bun cmd/format.ts -ts`.
- [ ] Run `bun tests/print-preview-dialog.ts --no-build`.
- [ ] Run `bun tests/run-almost-all.ts --no-build`.

**Commit checkpoint:** stop for explicit commit approval.

Suggested subject: `Test custom print preview dialog`

---

## Task 10: Full regression, performance check, and physical-print acceptance

**Files:**
- Create after measurements: `docs/superpowers/acceptance/2026-09-02-custom-print-preview-v1.md`
- Modify source/tests only if a measured failure requires a fix; each fix starts with a reproducing test where practical.

### Automated gate

- [ ] Run clang-format on all touched C/C++ files.
- [ ] Run `bun cmd/format.ts -ts` for touched TypeScript.
- [ ] Run `bun cmd/run-unit-tests.ts -dbg`.
- [ ] Run `bun cmd/build.ts -debug`.
- [ ] Run `bun tests/print-preview-dialog.ts --no-build`.
- [ ] Run `bun tests/run-almost-all.ts --no-build`.
- [ ] Run `bun tests/run-all.ts --no-build` to include Microsoft Print to PDF regressions.
- [ ] If the configured WSL toolchain is available, run `bun cmd/build.ts -wine`; otherwise record it as not run rather than treating absence of WSL as a product failure.

### Performance observations

Record measured values in the acceptance document:

- `Ctrl+P` -> dialog visible, target <= 500 ms on a normal local printer configuration;
- initial preview, target <= 1.0 s for a normal PDF;
- Fit/Actual update, target <= 500 ms;
- cached adjacent-page navigation, target <= 100 ms;
- uncached navigation, target <= 750 ms;
- no UI-thread stall > 100 ms attributable to page rasterization;
- preview cache remains <= 128 MB.

Targets are acceptance guidance, not reasons to hide an otherwise-correct preview. Record deviations with the fixture/printer used.

### Physical calibration gate

Use a calibration PDF containing known 100 mm and 200 mm lines. Test at least one physical printer; use A4 plus available A3/A1 devices where the printer supports them.

Record:

- printer model and driver;
- selected paper and driver DPI;
- document dimensions, printable dimensions, and preview-reported effective scale;
- measured 100 mm line length;
- measured 200 mm line length;
- Preview vs print orientation;
- Fit output;
- Custom 50% output;
- clipping warning vs observed hard-margin clipping;
- Properties Cancel leaves the main dialog unchanged;
- Properties OK rehydrates paper/orientation/duplex and preview.

Release criterion for Actual Size: measured physical lengths match requested lengths within normal printer/measuring tolerance; any systematic scale error is a release blocker.

### Final review

- [ ] Compare branch against `master`; confirm changes are limited to print-preview implementation, tests, resources, and design/acceptance docs.
- [ ] Verify `PrintWin11.cpp` remains available as explicit fallback.
- [ ] Verify Poster, N-up, Booklet, custom-paper creation, duplex sheet simulation, new annotation modes, and vendor finishing controls were not added.
- [ ] Verify no placeholder markers remain in implementation or acceptance documentation.
- [ ] Present the complete validation evidence and wait for explicit user approval before any final implementation commit/PR action required by repository policy.

**Final commit checkpoint:** explicit approval required.

Suggested subject: `Finish custom print preview v1`
