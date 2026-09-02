# SumatraPDF Custom Print Preview — Design

Date: 2026-09-02
Branch: `feat/custom-print-preview`
Repository: `sage1993/sumatrapdf_print`
Status: Approved design, implementation not started

## 1. Goal

Replace the default `Ctrl+P` flow in this personal SumatraPDF fork with a Sumatra-owned, Acrobat-style print dialog that provides a live page preview while preserving the existing SumatraPDF print engine and printer-driver integration.

The two release contracts are:

1. **Preview layout and actual printed layout use the same layout calculation.**
2. **Actual Size 100% is verified as a physical print scale, not only a UI label.**

This is a personal fork optimization. Upstream compatibility is desirable where inexpensive, but it is secondary to reliable Windows 11 printing for PDF and architectural-drawing workflows.

## 2. Scope

### 2.1 Included in v1

- `Ctrl+P` opens the custom Sumatra print dialog.
- Acrobat-like two-column information architecture.
- Printer selection.
- Copies.
- Page selection: all, current page, explicit ranges.
- Live preview with previous/next page navigation.
- Scaling modes:
  - Fit.
  - Actual Size.
  - Shrink oversized pages.
  - Custom scale percentage.
- Orientation:
  - Auto.
  - Portrait.
  - Landscape.
- Duplex enable/disable when supported.
- `Properties` button that opens the manufacturer's driver property UI.
- `Advanced` dialog for Sumatra-specific advanced settings.
- `Page Setup` dialog for paper, orientation, and paper source.
- Display of:
  - document physical dimensions;
  - paper dimensions;
  - printable-area dimensions;
  - effective scale;
  - output dimensions;
  - clipping diagnostics in millimetres.
- Explicit fallback action to the existing system print flow.
- Preview rendering off the UI thread.
- Bounded preview cache.

### 2.2 Deferred

- Poster / tiled printing.
- N-up / multiple pages per sheet.
- Booklet printing.
- Custom paper-size creation UI.
- Full duplex front/back sheet simulation.
- Manufacturer-specific features such as stapling, punching, account codes, secure print, or finishing controls.

Manufacturer-specific functions remain in the printer driver's own property UI.

## 3. Existing Code to Reuse

The fork already contains the Windows 11 preview work and the shared layout refactor.

Important existing components:

- `src/Print.cpp`
- `src/Print.h`
- `src/PrintWin11.cpp`
- `src/PrintWin11.h`
- `src/SumatraDialogs.cpp`
- `src/SumatraPDF.rc`
- `src/resource.h`

The authoritative existing layout function is:

```cpp
PrintPageLayout CalculatePrintPageLayout(
    EngineBase& engine,
    int pageNo,
    const Print_Advanced_Data& advanced,
    Size paperSize,
    Rect printable,
    float dpiX,
    float dpiY,
    bool printPortrait,
    Str printerName);
```

This function remains the single layout source for preview and printing. A separate preview-only Fit/center/rotation implementation is prohibited.

The existing `Printer` object and `NewPrinter()` already provide useful Win32 printer data via `OpenPrinterW()`, `DocumentPropertiesW()`, and `DeviceCapabilitiesW()`, including paper sizes, bins, color, duplex, collate, staple capability, orientation and `DEVMODE`.

## 4. UI Architecture

Use the existing SumatraPDF Win32 dialog pattern:

- resource-backed dialog definitions in `SumatraPDF.rc`;
- control IDs in `resource.h`;
- Win32 `DLGPROC` event handling;
- existing translation, RTL, theme, font-size and dark-mode infrastructure;
- a dedicated custom child HWND for the preview surface.

Do not implement a new UI framework.

### 4.1 Main dialog layout

The main dialog follows the Acrobat-style structure already approved:

- Top row:
  - printer ComboBox;
  - `Properties` button;
  - `Advanced` button;
  - copies control.
- Left column:
  - page range;
  - page scaling;
  - paper-source-by-page-size option;
  - duplex;
  - orientation;
  - annotation/form mode if supported by existing print contracts.
- Right column:
  - document size;
  - paper size;
  - live paper preview;
  - previous/next page controls;
  - page indicator;
  - effective scale;
  - output size;
  - printable size;
  - clipping status.
- Bottom row:
  - `Page Setup`;
  - `Print`;
  - `Cancel`.

`Poster`, `Multiple`, and `Booklet` tabs/buttons are not displayed in v1.

### 4.2 New source files

Preferred separation:

```text
src/
  Print.cpp
  Print.h
  PrintWin11.cpp
  PrintWin11.h
  PrintPreviewDialog.cpp        # new
  PrintPreviewDialog.h          # new
  PrintPreviewRenderer.cpp      # new
  PrintPreviewRenderer.h        # new
  SumatraDialogs.cpp
  SumatraPDF.rc
  resource.h
```

If implementation shows that renderer code is very small, `PrintPreviewRenderer.*` may remain part of `PrintPreviewDialog.*`; however, layout math must still stay outside UI code.

## 5. Dialog State Model

UI controls are not the source of truth. The dialog owns a canonical state object.

Conceptual model:

```cpp
struct PrintDialogState {
    PrinterSelection printer;
    int copies = 1;

    PageSelectionMode pageMode;
    Vec<PRINTPAGERANGE> ranges;
    int currentPreviewPage = 1;

    PrintScaleMode scaleMode;
    float customScalePercent = 100.0f;

    PrintOrientationMode orientation;
    bool duplex = false;
    bool paperSourceByPageSize = false;

    AnnotationPrintMode annotationMode;
    Print_Advanced_Data advanced;

    DEVMODEW* devMode = nullptr;
    uint64_t previewGeneration = 0;
};
```

Required conceptual enums:

```text
PageSelectionMode
  All
  Current
  Range

PrintScaleMode
  Fit
  ActualSize
  ShrinkOversized
  Custom

PrintOrientationMode
  Auto
  Portrait
  Landscape
```

Where fields overlap with existing `Print_Advanced_Data`, there must be one canonical value and an explicit conversion layer. Duplicated independent state is not allowed.

## 6. Printer Session and Driver Integration

Do not allow the UI to modify a live `Printer` or committed `DEVMODE` in place.

Use a transactional session model, conceptually:

```cpp
struct PrinterSession {
    Printer* printer = nullptr;
    DEVMODEW* committedDevMode = nullptr;
    DEVMODEW* workingDevMode = nullptr;
    PrinterMetrics metrics;
    PrinterCapabilities capabilities;
    uint64_t generation = 0;
};
```

### 6.1 Printer change transaction

When the user selects another printer:

1. Create a new printer object/session with `NewPrinter()` or its refactored equivalent.
2. Obtain/canonicalize `DEVMODE`.
3. Create the printer DC.
4. Read actual device geometry.
5. Validate the new session.
6. Commit only after all required steps succeed.
7. If any step fails, keep the previous printer session intact.

### 6.2 Manufacturer `Properties`

The `Properties` button opens the manufacturer's driver UI using a cloned working `DEVMODE` and `DocumentPropertiesW()` with prompt/input/output flags.

Required semantics:

- `OK`: validate returned `DEVMODE`, rebuild printer metrics, then commit and refresh UI/preview.
- `Cancel`: discard the working copy and preserve all existing state.
- Error: preserve existing state and display an error.

After `OK`, rehydrate the entire printer-derived UI state rather than attempting incremental field-by-field updates. The driver may have changed paper, orientation, duplex, color, tray, resolution, or vendor-private fields simultaneously.

### 6.3 Source-of-truth hierarchy

For geometry and preview:

1. **HDC / `GetDeviceCaps()` geometry** is authoritative.
2. `DEVMODE` describes the configured state.
3. `DeviceCapabilitiesW()` describes available capabilities.

Use these HDC values for canonical geometry:

- `PHYSICALWIDTH`
- `PHYSICALHEIGHT`
- `PHYSICALOFFSETX`
- `PHYSICALOFFSETY`
- `HORZRES`
- `VERTRES`
- `LOGPIXELSX`
- `LOGPIXELSY`

### 6.4 Paper and custom paper

Populate the paper UI from the printer's enumerated paper IDs/names/sizes.

V1 does not provide a custom-paper creation UI. If the manufacturer driver returns custom paper dimensions through `DEVMODE`, the dialog must still display and preview that custom size correctly.

### 6.5 Orientation

`Portrait` and `Landscape` map to `DEVMODE` orientation where appropriate.

`Auto` is a Sumatra-owned state and must not be faked as a third `DEVMODE` value. Auto orientation is resolved using document geometry and the shared layout calculation.

### 6.6 Duplex

If the printer does not report duplex support, disable the control.

V1 provides a simple duplex toggle and preserves driver defaults/orientation details where practical. Advanced long-edge/short-edge and vendor-specific controls stay in `Properties`.

## 7. Coordinate Systems and Printer Metrics

Keep four coordinate spaces distinct:

1. PDF document space.
2. Printer device space (canonical layout coordinates).
3. Physical display/diagnostic space in millimetres.
4. Preview viewport pixels.

### 7.1 Canonical coordinate system

**Printer device pixels are canonical for layout.**

Millimetres are derived for display, clipping reports, paper-size recognition and diagnostics only.

Do not repeatedly round-trip `px -> mm -> px` during layout.

### 7.2 PrinterMetrics

Conceptual model:

```cpp
struct PrinterMetrics {
    Size paperPx;
    Rect printablePx;
    float dpiX;
    float dpiY;

    SizeF paperMm;
    RectF printableMm;

    int physicalOffsetX;
    int physicalOffsetY;

    WORD paperId;
    Str paperName;
    bool portrait;
    bool duplexSupported;
    uint64_t hash;
};
```

Conversions for diagnostics/display:

```text
mmX = pxX * 25.4 / dpiX
mmY = pxY * 25.4 / dpiY
```

Never assume `dpiX == dpiY`; printers such as 600 x 1200 dpi devices must remain valid.

## 8. Preview Layout Contract

The preview must call the same `CalculatePrintPageLayout()` used by actual printing.

For non-stretch rendering, use the same target placement rule as the existing Win11 path:

```text
target.x = printable.x + layout.offset.x
target.y = printable.y + layout.offset.y
```

For stretch, use the printable-area rectangle.

Do not implement preview-only centering, Fit, rotation, shrink or physical-scale formulas.

### 8.1 Actual Size meaning

`Actual Size 100%` means:

```text
physical document length : physical printed length = 1 : 1
```

It does not mean that the preview appears at literal paper size on the monitor.

The UI displays `100.000 %` when actual physical scale resolves to 1.0.

In diagnostic/test data, keep requested and effective scale independently observable.

## 9. Preview Renderer

The preview renders the whole sheet context, not only a PDF thumbnail.

Layer order:

1. preview background;
2. paper shadow;
3. physical paper;
4. non-printable region;
5. printable-area boundary;
6. document bitmap;
7. clipping indication;
8. optional diagnostic overlay.

The right panel displays, at minimum:

- document width/height in mm;
- paper name and width/height in mm;
- printable width/height in mm;
- effective print scale;
- output dimensions;
- clipping status.

### 9.1 Preview raster DPI

Printer DPI controls physical layout calculations. Preview bitmap DPI controls only screen quality.

V1 policy:

- typical preview: 144 dpi;
- low/small viewport: as low as 96 dpi when useful;
- large preview: up to 192 dpi;
- hard preview cap: 192 dpi unless measurements prove a higher value is required.

Never render A1/A0 previews at the printer's 600/1200 dpi solely for screen display.

## 10. Clipping Diagnostics

Distinguish two conditions:

1. page bounds extend beyond the printable area;
2. actual content extends beyond the printable area.

Conceptual result:

```cpp
struct ClippingReport {
    bool pageBoundsOutsidePrintable = false;
    bool contentOutsidePrintable = false;
    float leftMm = 0;
    float rightMm = 0;
    float topMm = 0;
    float bottomMm = 0;
};
```

The UI must distinguish a harmless page-box overhang from likely clipping of real drawing/text/image content.

Example warning:

```text
Output content may be clipped
Left   2.37 mm
Right  0.00 mm
Top    0.00 mm
Bottom 1.82 mm
```

## 11. Preview Update Policy

Split changes into three update levels.

### Level 0: no preview rerender

Examples:

- copies;
- duplex when preview sheet simulation is not implemented;
- settings that do not affect page geometry/content rendering.

### Level 1: rerender current page

Examples:

- Fit;
- Actual Size;
- Shrink;
- Custom scale;
- orientation;
- auto-rotation-related changes.

### Level 2: invalidate printer context/cache

Examples:

- printer change;
- paper-size change;
- manufacturer property changes;
- printable-area change;
- printer resolution/geometry change.

## 12. Asynchronous Rendering

Preview rendering must not block the UI thread.

Use generation-based invalidation:

1. Increment `previewGeneration` whenever preview-affecting state changes.
2. Submit an immutable render job snapshot to a worker.
3. Tag the result with its generation.
4. Display only if the result generation still equals the current generation.
5. Discard stale results.

Conceptual job/result:

```cpp
struct PrintPreviewJob {
    uint64_t generation;
    int pageNo;
    PrinterMetrics metrics;
    Print_Advanced_Data advanced;
    PrintOrientationMode orientation;
    float customScale;
    Size viewportSize;
    float rasterDpi;
};

struct PrintPreviewResult {
    uint64_t generation;
    int pageNo;
    PreviewBitmap bitmap;
    PrintPageLayout layout;
    Rect pageTarget;
    ClippingReport clipping;
    PreviewMetadata metadata;
};
```

Worker code must not dereference transient UI HWND/control state.

### 12.1 Custom scale debounce

Do not rerender on every keystroke while a scale value is being edited.

Initial policy: approximately 150 ms debounce, followed by validation and rerender.

Accepted UI input range for v1: 1.0% to 1000.0%.

## 13. Preview Cache

Default cache window:

```text
page N-1
page N
page N+1
```

Preload the next page after navigation where cheap.

Default total preview-bitmap cache budget: 128 MB. Evict least-recently-used/oldest preview bitmaps when over budget.

## 14. Page Setup

V1 Page Setup contains:

- paper size;
- portrait/landscape;
- paper source/bin where available.

Do not add user-entered top/left/right/bottom margins in v1. Printer hard margins come from actual HDC geometry, while document positioning is handled through the print scaling/centering rules.

## 15. Advanced Dialog

Reuse and reorganize existing Sumatra advanced print options where possible rather than duplicating state.

V1 may expose options such as:

- even/odd/all page filtering when compatible with the main range model;
- paper source by page size;
- mixed-size per-page paper handling;
- extra rotation;
- compatibility/image-printing controls that already exist or are required for reliable printing.

Any new advanced option must have a single canonical state and a defined effect on preview invalidation.

## 16. Fallback Policy

The custom dialog is the default `Ctrl+P` flow.

Do not silently jump to the system dialog on preview failure.

On recoverable custom-dialog failure, offer explicit actions:

```text
Preview could not be generated.
[Retry] [Use system print] [Cancel]
```

`Use system print` invokes an existing proven path (`PrintDlgEx` and/or the existing Win11 path as appropriate).

Keep `PrintWin11.cpp` in v1 as a fallback/reference implementation. Do not delete it during the initial feature implementation.

## 17. Validation Before Actual Print

When the user presses `Print`:

1. Validate the current printer session again.
2. Build an immutable print-job snapshot.
3. Ensure required printer/DC geometry is valid.
4. Only then close the custom dialog and start the existing print path.

Conceptual snapshot:

```cpp
struct PrintJobSettings {
    Printer* printer;
    DEVMODEW* devMode;
    Vec<PRINTPAGERANGE> ranges;
    Print_Advanced_Data advanced;
    int copies;
    bool duplex;
};
```

A printer that disappeared or became invalid while the dialog was open must not start a print job with stale state.

## 18. Error Handling

Required behavior:

| Failure | Required behavior |
| --- | --- |
| `OpenPrinterW()` fails | Keep previous printer state. |
| `NewPrinter()` / session build fails | Reject new selection. |
| Manufacturer Properties cancelled | No state change. |
| Manufacturer Properties errors | Keep previous state and show an error. |
| `CreateDC()` fails | Do not commit the new session. |
| Invalid DPI / geometry | Reject session or use only an already-proven existing safe fallback. |
| Invalid printable area | Do not allow unsafe print from the custom path. |
| Preview render fails | Keep dialog open; offer retry/system-print/cancel. |
| Async stale preview completes | Discard it. |
| Print start fails | Reuse existing `PrintResult`/error-reporting contracts. |

Existing defensive zoom/geometry handling such as `SanitizePrintZoom()` remains in place.

## 19. Testing Strategy

Testing is split into four levels.

### L1 — layout/math contract tests

Test the shared layout and clipping math without relying on a physical printer.

Representative matrix:

| Document | Paper | Mode | Orientation | Expected |
| --- | --- | --- | --- | --- |
| A4 portrait | A4 | Actual | Portrait | 100% |
| A4 landscape | A4 | Actual | Auto | auto-rotated, 100% |
| A3 | A4 | Fit | Auto | reduced |
| A4 | A3 | Shrink | Auto | remains 100% |
| A4 | A3 | Fit | Auto | enlarged |
| A3 | A3 | Custom 50% | Landscape | exactly 50% |
| A1 | A3 | Actual | Auto | clipping reported |
| A1 | A1 | Actual | Auto | 100% |

Core assertions:

- preview and actual print use the same `PrintPageLayout` inputs/results;
- Actual Size effective physical scale equals 1.0 within numeric tolerance;
- non-square DPI remains valid;
- clipping sides are computed correctly.

Suggested tolerances:

- clipping/display measurements: ±0.1 mm;
- paper-size recognition/driver rounding: ±0.5 mm.

### L2 — printer/DEVMODE state tests

Introduce a thin testable Win32 printer-platform boundary only as needed for deterministic tests.

Test at least:

- properties `OK` commits;
- properties `Cancel` preserves old `DEVMODE`;
- printer-change `CreateDC` failure preserves previous printer;
- invalid printable geometry is rejected;
- duplex unsupported disables the control;
- property changes fully rehydrate paper/orientation/duplex state.

Avoid unrelated refactoring of the whole printing subsystem.

### L3 — preview/UI tests

Prefer geometry/state assertions over fragile pixel-perfect snapshots.

Assert:

- physical paper rectangle;
- printable rectangle;
- document target rectangle;
- rotation;
- effective scale;
- clipping data;
- generation discard behavior.

Use a small visual-smoke fixture set for screenshot/human review:

- A4 portrait;
- A3 landscape;
- A1 -> A3 Fit;
- A1 -> A1 Actual Size;
- deliberately clipped drawing.

### L4 — physical/manual acceptance

At least one real printer or trusted PDF-printer run is required before calling v1 complete.

Use calibration fixtures containing known 100 mm and 200 mm lines.

Required checks:

- A4 -> A4 Actual prints 100 mm as 100 mm;
- A3 -> A3 Actual prints to physical scale;
- A1 -> A1 Actual where hardware is available;
- A3 -> A4 Fit matches preview placement;
- Auto landscape output matches preview;
- Custom 50% measures as 50%;
- clipping corresponds to preview;
- duplex setting reaches the driver;
- Properties Cancel changes nothing.

## 20. Performance and Memory Gates

V1 targets:

| Operation | Target |
| --- | ---: |
| `Ctrl+P` -> dialog visible | <= 500 ms recommended |
| Initial preview, ordinary PDF | <= 1.0 s |
| Fit/Actual change, ordinary page | <= 500 ms |
| Cached page navigation | <= 100 ms |
| Uncached page navigation | <= 750 ms recommended |
| Long UI-thread blocking work | avoid > 100 ms |
| Default preview raster | 144 dpi |
| Preview raster cap | 192 dpi |
| Preview cache | N-1, N, N+1 |
| Preview-bitmap cache budget | 128 MB |

Large architectural sheets must not allocate preview images at the printer's full high-resolution DPI.

## 21. Release Acceptance Criteria

V1 is complete only when all of the following are satisfied:

1. `Ctrl+P` opens the custom print dialog rather than the Windows dialog.
2. Initial preview appears for a normal PDF.
3. All/current/range selection works.
4. Actual/Fit/Shrink/Custom scale modes work.
5. Actual Size is physically 100% in calibration output.
6. Preview and real print share the same layout calculation.
7. A1/A2/A3/A4 paper geometry and orientation are correct where the selected printer supports them.
8. Printable area is visible and clipping diagnostics are correct.
9. Printer changes are transactional.
10. Manufacturer Properties `OK` and `Cancel` behave transactionally.
11. Properties changes rehydrate the UI and preview from fresh printer geometry.
12. Preview rendering does not block the UI thread for long work.
13. Stale async preview results cannot overwrite newer state.
14. Preview memory is bounded for large documents.
15. Explicit system-print fallback works.
16. Existing command-line and non-interactive printing remain unaffected unless deliberately routed through shared refactoring.
17. Existing `PrintWin11` fallback/reference path remains buildable in v1.
18. Physical calibration confirms the scale contract.

## 22. Explicit Non-Goals and Guardrails

Do not:

- recreate manufacturer printer property pages;
- add Poster/N-up/Booklet in v1;
- create a separate preview layout algorithm;
- compute Actual Size from monitor DPI;
- assume square printer DPI;
- substitute standard A-series margins for the driver's reported printable area;
- use mm as the iterative canonical layout coordinate;
- render screen previews at 600/1200 printer DPI;
- silently commit partially validated printer changes;
- silently fall back to the system print dialog after a custom-preview error.

## 23. Implementation Sequence Constraint

The implementation plan must preserve this dependency order:

1. Add testable state/geometry contracts.
2. Introduce printer-session transaction handling.
3. Add preview renderer using shared layout math.
4. Add async generation/caching.
5. Add the main resource dialog and bind controls to canonical state.
6. Add Properties/Page Setup/Advanced integration.
7. Route `Ctrl+P` to the custom dialog while preserving explicit fallback.
8. Run automated regression tests.
9. Run real-print calibration acceptance.

Implementation must follow TDD for behavior changes and must verify existing printing paths before completion claims.
