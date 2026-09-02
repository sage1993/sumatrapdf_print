# SumatraPDF Custom Print Preview — Design

Date: 2026-09-02
Branch: `feat/custom-print-preview`
Repository: `sage1993/sumatrapdf_print`
Status: Approved design; implementation not started

## 1. Goal

Replace the default `Ctrl+P` flow in this personal SumatraPDF fork with a Sumatra-owned, Acrobat-style print dialog that provides a live page preview while preserving the existing SumatraPDF print engine and printer-driver integration.

The release contracts are:

1. **Preview layout and actual printed layout use the same layout calculation.**
2. **Actual Size 100% is verified as a physical print scale, not only a UI label.**
3. **Printer-driver state changes are transactional: invalid or cancelled changes never partially mutate the active print state.**

This is a personal-fork optimization. Upstream compatibility is desirable where inexpensive, but secondary to reliable Windows 11 PDF and architectural-drawing printing.

## 2. V1 Scope

### Included

- `Ctrl+P` opens the custom Sumatra print dialog.
- Acrobat-like two-column information architecture.
- Printer selection.
- Copies.
- Page selection: all, current page, explicit ranges.
- Live preview with previous/next navigation.
- Scaling:
  - Fit.
  - Actual Size.
  - Shrink oversized pages.
  - Custom scale percentage.
- Orientation:
  - Auto.
  - Portrait.
  - Landscape.
- Duplex toggle when supported.
- `Properties` button for the manufacturer's driver UI.
- `Advanced` dialog for Sumatra-specific advanced settings that already have backend contracts or are explicitly added as part of this feature.
- `Page Setup` for paper, orientation, and paper source.
- Display of:
  - document physical dimensions;
  - paper dimensions;
  - printable-area dimensions;
  - effective scale;
  - output dimensions;
  - clipping diagnostics in millimetres.
- Explicit fallback to the existing system print flow.
- Preview rendering off the UI thread.
- Bounded preview cache.

### Deferred

- Poster / tiled printing.
- N-up / multiple pages per sheet.
- Booklet printing.
- Custom paper-size creation UI.
- Full duplex front/back sheet simulation.
- New annotation/form printing modes.
- Manufacturer-specific features such as stapling, punching, account codes, secure print, and finishing controls.

Existing annotation/markup printing behavior remains unchanged in v1. The custom dialog does not introduce a new annotation-mode selector unless the existing backend exposes a stable setting that can be reused without expanding scope.

Manufacturer-specific functions remain in the printer driver's own property UI.

## 3. Existing Code to Reuse

Important existing components:

- `src/Print.cpp`
- `src/Print.h`
- `src/PrintWin11.cpp`
- `src/PrintWin11.h`
- `src/SumatraDialogs.cpp`
- `src/SumatraPDF.rc`
- `src/resource.h`

The current authoritative layout function is:

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

This shared layout path remains authoritative for preview and actual printing.

### 3.1 Required extension for Custom Scale

The existing layout contract does not directly represent an arbitrary user-entered percentage. V1 therefore extends the shared layout contract rather than creating a preview-only scale path.

Preferred design:

```cpp
struct PrintScaleSpec {
    PrintScaleAdv mode;
    float customPercent = 100.0f;
};
```

`CalculatePrintPageLayout()` is extended, directly or through a closely related shared helper, so **all callers** use the same `PrintScaleSpec` semantics.

Required behavior:

- `ActualSize` / existing `None`: physical scale 1.0.
- `Fit`: existing Fit behavior.
- `ShrinkOversized`: existing Shrink behavior.
- `Custom`: physical scale = `customPercent / 100.0` relative to Actual Size.

There must not be a separate `PreviewCustomScale()` implementation.

The exact type placement may change during implementation to fit existing code style, but the single-source-of-truth requirement is fixed.

The existing `Printer` object and `NewPrinter()` already provide useful Win32 printer data via `OpenPrinterW()`, `DocumentPropertiesW()`, and `DeviceCapabilitiesW()`, including paper sizes, bins, color, duplex, collate, staple capability, orientation, and `DEVMODE`.

## 4. UI Architecture

Use the existing SumatraPDF Win32 dialog pattern:

- resource-backed dialogs in `SumatraPDF.rc`;
- control IDs in `resource.h`;
- Win32 `DLGPROC` event handling;
- existing translation, RTL, theme, font-size, and dark-mode infrastructure;
- a dedicated custom child HWND for the preview surface.

Do not introduce a new UI framework.

### 4.1 Main dialog layout

Top row:

- printer ComboBox;
- `Properties`;
- `Advanced`;
- copies control.

Left column:

- page range;
- scaling mode;
- custom percentage when Custom is selected;
- paper-source-by-page-size option where compatible with existing behavior;
- duplex;
- orientation.

Right column:

- document size;
- paper size;
- live sheet preview;
- previous/next page controls;
- page indicator;
- effective scale;
- output size;
- printable size;
- clipping status.

Bottom row:

- `Page Setup`;
- `Print`;
- `Cancel`.

`Poster`, `Multiple`, and `Booklet` controls are not displayed in v1.

### 4.2 Preferred source separation

```text
src/
  Print.cpp
  Print.h
  PrintWin11.cpp
  PrintWin11.h
  PrintPreviewDialog.cpp        # new
  PrintPreviewDialog.h          # new
  PrintPreviewRenderer.cpp      # new, unless proven small enough to co-locate
  PrintPreviewRenderer.h        # new, unless proven small enough to co-locate
  SumatraDialogs.cpp
  SumatraPDF.rc
  resource.h
```

Layout math stays outside UI code even if the renderer files are co-located with the dialog implementation.

## 5. Canonical Dialog State

UI controls are views over a canonical state object.

Conceptual model:

```cpp
struct PrintDialogState {
    PrinterSelection printer;
    int copies = 1;

    PageSelectionMode pageMode;
    Vec<PRINTPAGERANGE> ranges;
    int currentPreviewPage = 1;

    PrintScaleSpec scale;
    PrintOrientationMode orientation;

    bool duplex = false;
    bool paperSourceByPageSize = false;

    Print_Advanced_Data advanced;
    DEVMODEW* devMode = nullptr;

    uint64_t previewGeneration = 0;
};
```

Conceptual enums:

```text
PageSelectionMode
  All
  Current
  Range

PrintOrientationMode
  Auto
  Portrait
  Landscape
```

Where `PrintDialogState`, `PrintScaleSpec`, and existing `Print_Advanced_Data` overlap, there must be one canonical value plus explicit conversion. Duplicated independently mutable state is prohibited.

## 6. Printer Session and Driver Integration

Do not allow UI controls to mutate a committed `Printer` or `DEVMODE` in place.

Conceptual transactional session:

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

1. Create a candidate printer/session.
2. Obtain and canonicalize its `DEVMODE`.
3. Create the printer DC.
4. Read actual device geometry.
5. Validate the candidate session.
6. Commit only after all required steps succeed.
7. If any step fails, preserve the previous session unchanged.

### 6.2 Manufacturer `Properties`

Use a cloned working `DEVMODE` with `DocumentPropertiesW()` prompt/input/output behavior.

- `OK`: validate, rebuild metrics/capabilities, commit, fully rehydrate UI, invalidate preview.
- `Cancel`: discard working state, no visible state change.
- Error: preserve committed state and show an error.

After `OK`, rebuild all printer-derived UI state because the driver may change paper, orientation, duplex, color, tray, resolution, and vendor-private fields together.

### 6.3 Source-of-truth hierarchy

For geometry and preview:

1. **HDC / `GetDeviceCaps()` geometry** — authoritative.
2. `DEVMODE` — configured state.
3. `DeviceCapabilitiesW()` — available capabilities.

Canonical geometry uses:

- `PHYSICALWIDTH`
- `PHYSICALHEIGHT`
- `PHYSICALOFFSETX`
- `PHYSICALOFFSETY`
- `HORZRES`
- `VERTRES`
- `LOGPIXELSX`
- `LOGPIXELSY`

### 6.4 Paper and custom paper

Populate paper choices from enumerated printer paper IDs/names/sizes.

V1 does not create custom paper sizes. If the driver returns a custom paper through `DEVMODE`, display and preview it correctly.

### 6.5 Orientation

`Portrait` and `Landscape` map to normal driver orientation state.

`Auto` is Sumatra-owned state. It is not represented as a fake third `DEVMODE` orientation and is resolved using document geometry plus the shared layout path.

### 6.6 Duplex

If the printer does not report duplex support, disable the control.

V1 provides a simple duplex toggle. Long-edge/short-edge and vendor-specific behavior remains in `Properties` unless the current driver state can be safely preserved without additional UI.

## 7. Coordinate Systems and Printer Metrics

Keep four spaces distinct:

1. PDF document space.
2. Printer device space.
3. Physical diagnostic/display space in millimetres.
4. Preview viewport pixels.

### 7.1 Canonical coordinates

**Printer device pixels are canonical for layout.**

Millimetres are derived for display, clipping reports, paper recognition, and diagnostics only.

Do not round-trip `px -> mm -> px` during layout.

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

Display conversion:

```text
mmX = pxX * 25.4 / dpiX
mmY = pxY * 25.4 / dpiY
```

Never assume `dpiX == dpiY`.

## 8. Preview Layout Contract

Preview and real printing call the same shared layout path.

For non-stretch placement, preserve the existing Win11 rule:

```text
target.x = printable.x + layout.offset.x
target.y = printable.y + layout.offset.y
```

For stretch, use the printable-area rectangle.

No preview-only centering, Fit, rotation, Shrink, Actual Size, or Custom Scale formulas are allowed.

### 8.1 Actual Size

`Actual Size 100%` means:

```text
physical document length : physical printed length = 1 : 1
```

It does not mean literal paper size on the monitor.

The normal UI displays the resolved effective scale, e.g. `100.000 %`. Diagnostic/test data must make requested and effective scale separately observable.

## 9. Preview Renderer

Render the sheet context, not only a document thumbnail.

Layer order:

1. preview background;
2. paper shadow;
3. physical paper;
4. non-printable region;
5. printable-area boundary;
6. document bitmap;
7. clipping indication;
8. optional diagnostic overlay.

The right panel shows at least:

- document dimensions;
- paper name and dimensions;
- printable dimensions;
- effective scale;
- output dimensions;
- clipping status.

### 9.1 Preview raster DPI

Printer DPI controls physical layout. Preview DPI controls screen quality only.

V1 policy:

- default: 144 dpi;
- low/small viewport: 96–144 dpi;
- large viewport: up to 192 dpi;
- hard preview cap: 192 dpi unless measurement proves it insufficient.

Never rasterize screen previews at the printer's 600/1200 dpi solely for display.

## 10. Clipping Diagnostics

Distinguish:

1. page bounds outside printable area;
2. actual content outside printable area.

Conceptual model:

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

The UI distinguishes harmless page-box overhang from likely clipping of actual drawing/text/image content.

## 11. Preview Update Policy

### Level 0 — no rerender

Examples:

- copies;
- duplex while sheet-side simulation is absent;
- settings that do not change geometry/content rendering.

### Level 1 — rerender current page

Examples:

- Fit;
- Actual Size;
- Shrink;
- Custom percentage;
- orientation;
- auto-rotation-related changes.

### Level 2 — invalidate printer context/cache

Examples:

- printer change;
- paper change;
- manufacturer property changes;
- printable-area change;
- resolution/geometry change.

## 12. Asynchronous Rendering

Preview rendering must not perform long work on the UI thread.

Use generation invalidation:

1. Increment `previewGeneration` on preview-affecting state changes.
2. Submit an immutable job snapshot.
3. Tag results with generation.
4. Display only if result generation equals current generation.
5. Discard stale results.

Conceptual data:

```cpp
struct PrintPreviewJob {
    uint64_t generation;
    int pageNo;
    PrinterMetrics metrics;
    Print_Advanced_Data advanced;
    PrintScaleSpec scale;
    PrintOrientationMode orientation;
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

Worker code must not read transient UI HWND/control state.

### 12.1 Custom percentage debounce

Initial policy: approximately 150 ms after input settles, then validate and rerender.

Accepted v1 input range: 1.0% through 1000.0%.

## 13. Preview Cache

Default working set:

```text
page N-1
page N
page N+1
```

Preload the likely next page where cheap.

Default bitmap cache budget: 128 MB. Evict old/least-recent entries over budget.

## 14. Page Setup

V1 contains:

- paper size;
- portrait/landscape;
- paper source/bin where available.

Do not add user-entered page margins in v1. Printer hard margins come from HDC geometry; document placement comes from scaling/centering rules.

## 15. Advanced Dialog

Reuse and reorganize existing Sumatra advanced print settings rather than duplicating state.

Candidate v1 settings are limited to options with an existing or explicitly added backend contract, such as:

- even/odd/all filtering where compatible with the main range model;
- paper source by page size;
- mixed per-page paper handling;
- extra rotation;
- existing compatibility/image-printing behavior.

Every advanced setting must have one canonical state and a defined preview invalidation level.

## 16. Fallback Policy

The custom dialog is the default `Ctrl+P` flow.

Do not silently switch to a system dialog after a preview error.

On recoverable failure, offer:

```text
Preview could not be generated.
[Retry] [Use system print] [Cancel]
```

`Use system print` invokes an existing proven print-dialog path.

Keep `PrintWin11.cpp` in v1 as fallback/reference code. Do not delete it during initial implementation.

## 17. Validation Before Actual Print

On `Print`:

1. Revalidate the current printer session.
2. Build an immutable print-job snapshot.
3. Ensure required printer/DC geometry remains valid.
4. Only then close the custom dialog and invoke the existing print path.

Conceptual snapshot:

```cpp
struct PrintJobSettings {
    Printer* printer;
    DEVMODEW* devMode;
    Vec<PRINTPAGERANGE> ranges;
    Print_Advanced_Data advanced;
    PrintScaleSpec scale;
    int copies;
    bool duplex;
};
```

If a printer disappeared or became invalid while the dialog was open, do not print with stale state.

## 18. Error Handling

| Failure | Required behavior |
| --- | --- |
| `OpenPrinterW()` fails | Keep previous printer state. |
| candidate session build fails | Reject new selection. |
| manufacturer Properties cancelled | No state change. |
| manufacturer Properties errors | Keep previous state and show an error. |
| `CreateDC()` fails | Do not commit candidate state. |
| invalid DPI / geometry | Reject candidate or use only an already-proven safe fallback. |
| invalid printable area | Do not allow unsafe print from the custom path. |
| preview render fails | Keep dialog open; offer retry/system-print/cancel. |
| stale async preview completes | Discard it. |
| print start fails | Reuse existing `PrintResult`/error-reporting contracts. |

Existing defensive zoom/geometry handling such as `SanitizePrintZoom()` remains in place.

## 19. Testing Strategy

Testing has four levels.

### L1 — Layout/math contract

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

- preview and actual print use the same `PrintPageLayout` contract;
- Actual Size effective physical scale equals 1.0 within numeric tolerance;
- Custom percentage is derived from Actual Size in the shared layout path;
- non-square DPI is valid;
- clipping sides are correct.

Suggested tolerances:

- clipping/display measurement: ±0.1 mm;
- paper recognition/driver integer rounding: ±0.5 mm.

### L2 — Printer/DEVMODE state

Introduce only the minimum Win32 printer-platform boundary needed for deterministic tests.

Test at least:

- Properties `OK` commits;
- Properties `Cancel` preserves state;
- printer-change `CreateDC` failure preserves previous printer;
- invalid printable geometry is rejected;
- duplex unsupported disables the control;
- property changes fully rehydrate paper/orientation/duplex state.

Avoid unrelated printing-subsystem refactors.

### L3 — Preview/UI

Prefer geometry/state assertions over fragile pixel-perfect snapshots.

Assert:

- physical paper rectangle;
- printable rectangle;
- document target rectangle;
- rotation;
- effective scale;
- clipping data;
- generation discard behavior.

Visual-smoke fixtures:

- A4 portrait;
- A3 landscape;
- A1 -> A3 Fit;
- A1 -> A1 Actual;
- deliberately clipped drawing.

### L4 — Physical/manual acceptance

At least one real printer or trusted PDF-printer run is required before v1 completion.

Calibration fixtures contain known 100 mm and 200 mm lines.

Required checks:

- A4 -> A4 Actual: 100 mm prints as 100 mm;
- A3 -> A3 Actual: physical scale is correct;
- A1 -> A1 Actual where hardware permits;
- A3 -> A4 Fit matches preview placement;
- Auto landscape matches preview;
- Custom 50% measures as 50%;
- clipping corresponds to preview;
- duplex state reaches the driver;
- Properties Cancel changes nothing.

## 20. Performance and Memory Gates

| Operation | Target |
| --- | ---: |
| `Ctrl+P` -> dialog visible | <= 500 ms recommended |
| initial preview, ordinary PDF | <= 1.0 s |
| Fit/Actual/Custom update, ordinary page | <= 500 ms |
| cached navigation | <= 100 ms |
| uncached navigation | <= 750 ms recommended |
| long UI-thread blocking work | avoid > 100 ms |
| default preview raster | 144 dpi |
| preview raster cap | 192 dpi |
| cache window | N-1, N, N+1 |
| bitmap cache budget | 128 MB |

Large architectural sheets must not allocate screen-preview images at full printer DPI.

## 21. Release Acceptance Criteria

V1 is complete only when all of the following are satisfied:

1. `Ctrl+P` opens the custom dialog.
2. Initial preview appears for a normal PDF.
3. All/current/range selection works.
4. Actual/Fit/Shrink/Custom work.
5. Actual Size is physically 100% in calibration output.
6. Custom percentage uses the same shared layout path as actual print.
7. Preview and actual print share the same layout calculation.
8. A1/A2/A3/A4 geometry and orientation are correct where supported by the selected printer.
9. Printable area and clipping diagnostics are correct.
10. Printer changes are transactional.
11. Manufacturer Properties `OK` and `Cancel` are transactional.
12. Properties changes rebuild UI/preview from fresh printer geometry.
13. Preview rendering avoids long UI-thread blocking.
14. Stale async results cannot overwrite newer state.
15. Preview memory is bounded.
16. Explicit system-print fallback works.
17. Existing command-line and non-interactive printing remain unaffected except for deliberately shared internal refactors.
18. Existing `PrintWin11` fallback/reference path remains buildable in v1.
19. Physical calibration confirms the scale contract.

## 22. Guardrails

Do not:

- recreate manufacturer property pages;
- add Poster/N-up/Booklet in v1;
- add new annotation/form-printing modes in v1;
- create a preview-only layout algorithm;
- create a preview-only Custom Scale algorithm;
- compute Actual Size from monitor DPI;
- assume square printer DPI;
- substitute standard A-series margins for driver-reported printable geometry;
- use mm as the iterative canonical layout coordinate;
- rasterize screen previews at 600/1200 printer DPI;
- silently commit partially validated printer changes;
- silently fall back to the system print dialog after a custom-preview error.

## 23. Implementation Dependency Order

The implementation plan must preserve this order:

1. Add/extend testable shared scale and geometry contracts, including Custom Scale.
2. Add printer-session transaction handling.
3. Add preview renderer using shared layout math.
4. Add async generation/caching.
5. Add the main resource dialog and bind controls to canonical state.
6. Add Properties/Page Setup/Advanced integration.
7. Route `Ctrl+P` to the custom dialog while preserving explicit fallback.
8. Run automated regression tests.
9. Run real-print calibration acceptance.

Implementation follows TDD for behavior changes and verifies existing printing paths before completion claims.
