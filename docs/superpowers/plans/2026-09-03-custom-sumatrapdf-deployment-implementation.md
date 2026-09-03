# Custom SumatraPDF Deployment Implementation Plan

**Date:** 2026-09-03  
**Repository:** `sage1993/sumatrapdf_print`  
**Branch:** `feat/custom-print-preview`  
**Approved design:** `docs/superpowers/specs/2026-09-03-custom-sumatrapdf-deployment-design.md`  
**Execution style:** test-driven, one reviewable checkpoint per task

## Objective

Implement a managed custom SumatraPDF deployment path that upgrades an existing installed SumatraPDF in place, preserves the existing runtime/settings identity, prevents the official updater from replacing the custom build, preserves the user's PDF default-app choice, and restores the prior runnable application if an upgrade fails after destructive replacement begins.

The implementation extends the existing SumatraPDF installer/uninstaller/update infrastructure. It does not introduce a parallel installer product, change `kAppName`, rename `SumatraPDF.exe`, modify Windows `UserChoice`, or add a custom automatic updater.

## Baseline and Verification Commands

Use the repository's existing Windows build scripts and the same commands used by the completed Print Preview work:

```powershell
bun cmd/run-unit-tests.ts -dbg
bun cmd/build.ts -debug
```

Before every task checkpoint:

```powershell
git diff --check
```

When a task changes generated Visual Studio project membership, regenerate through the repository's normal premake flow before the build. Production acceptance additionally requires a release installer build and the DP matrix in Task 11.

The GitHub connector used to prepare this plan cannot execute the Windows toolchain, registry, Restart Manager, installer UI, reboot, default-app, or physical-printer tests. Such checks remain `NOT_RUN` until executed in the Windows workspace.

## Implementation Invariants

Every task must preserve these invariants:

1. `kAppName` remains exactly `"SumatraPDF"`.
2. The installed executable remains `SumatraPDF.exe`.
3. Existing installed-mode LocalAppData remains the user-state namespace.
4. No installer path writes Windows `UserChoice`.
5. A non-SumatraPDF PDF default must not be taken over.
6. The custom build cannot use the official update metadata/download/installer path.
7. No running SumatraPDF process is force-terminated before explicit user consent.
8. The live settings file is never parsed-and-rewritten as part of deployment.
9. Failure after destructive replacement triggers restoration of the prior application files and changed registration state.
10. Custom uninstall does not delete installed-mode settings, favorites, or history.

---

## Task 1: Add distribution identity without changing runtime identity

**Traceability:** Design 5.1, 5.2; DP-01, DP-02, DP-08, DP-12  
**Commit:** `feat: add custom distribution identity`

**Files:**
- Modify: `src/Version.h`
- Modify: `src/SumatraPDF.rc`
- Modify the existing source that renders Help/About after locating the current handler in this branch; do not create a second About dialog.
- Add/modify a narrow debug unit-test source only if compile-time assertions cannot cover the identity contract cleanly.

### Contract

Add separate distribution constants while preserving the runtime application name:

```cpp
#define kAppName "SumatraPDF"
#define kCustomDistributionName "SumatraPDF Custom"
#define kCustomDistributionId "sage1993.sumatrapdf_print"
#define kCustomDistributionVersion "custom.1"
```

Provide one composed custom display version derived from the upstream base, for example `3.7-custom.1`. Numeric `FILEVERSION` and `PRODUCTVERSION` continue to use `CURR_VERSION_COMMA`; string VERSIONINFO metadata identifies the custom distribution.

### TDD steps

- [ ] RED: add compile-time/debug assertions proving `kAppName == "SumatraPDF"`, distribution id/name are non-empty, and the custom display version contains both the upstream and custom components.
- [ ] Run `bun cmd/run-unit-tests.ts -dbg`; record the expected failure before implementation.
- [ ] Add the distribution constants and composed version helper/macros in `Version.h`.
- [ ] Change only `FileDescription`, `ProductName`, and string version metadata in `SumatraPDF.rc`; keep numeric version resources upstream-compatible.
- [ ] Locate the existing Help/About renderer and display `SumatraPDF Custom`, custom distribution version, upstream base version, and build date using the existing UI path.
- [ ] Re-run `bun cmd/run-unit-tests.ts -dbg` and `bun cmd/build.ts -debug`.
- [ ] Inspect the built EXE VERSIONINFO and About UI in the Windows workspace.
- [ ] Run `git diff --check`.

**Checkpoint evidence:** runtime identity unchanged; custom display identity visible; build/tests PASS.

---

## Task 2: Hard-disable the official update channel

**Traceability:** Design 5.4; DP-12  
**Commit:** `feat: disable official updates for custom build`

**Files:**
- Modify: `src/UpdateCheck.h`
- Modify: `src/UpdateCheck.cpp`
- Modify: `src/Menu.cpp`
- Modify existing update-check unit tests or add a focused debug test file registered in the unit-test target.

### Contract

One distribution predicate is the source of truth, e.g. `IsCustomDistribution()`. The custom build must reject both scheduled and explicitly requested official update checks and must reject the official installer auto-upgrade path even if a caller bypasses the menu.

The Help menu must not expose `Check for Updates` for the custom distribution. Existing `checkForUpdates` settings remain schema-compatible but cannot override the hard gate.

### TDD steps

- [ ] RED: test that custom distribution update eligibility is false for both scheduled and user-initiated requests.
- [ ] RED: test that the official installer auto-upgrade entry predicate is false for the custom distribution.
- [ ] Run unit tests and capture the expected failures.
- [ ] Add the API/runtime hard gate at the earliest update-check entry point in `UpdateCheck.cpp`.
- [ ] Guard the official installer download/launch/auto-upgrade entry separately so a future caller cannot bypass the eligibility check.
- [ ] Hide/remove the Help-menu update command in `Menu.cpp` for the custom distribution.
- [ ] Preserve official upstream behavior behind the non-custom branch so the patch remains easy to audit.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows smoke: use network logging or debugger instrumentation to prove invoking all reachable update paths makes no request to the official update endpoint.

**Checkpoint evidence:** no official update UI, check, download, or installer launch path remains reachable in Custom.

---

## Task 3: Classify clean, official, custom, and portable installation states

**Traceability:** Design 5.3, 6.1-6.3; DP-01, DP-02, DP-08  
**Commit:** `feat: classify custom SumatraPDF installations`

**Files:**
- Modify: `src/Installer.h`
- Modify: `src/Installer.cpp`
- Add focused installer classification tests to the existing debug/unit-test harness.

### Contract

Extend `PreviousInstallationInfo` or introduce a small adjacent enum/struct:

```cpp
enum class PreviousInstallKind {
    None,
    OfficialOrLegacy,
    ManagedCustom,
    PortableOnly,
};
```

Classification must be data-driven and side-effect free. Installed path detection remains based on the existing installer/registry logic; the custom marker only distinguishes official/legacy from managed custom after an installed copy has been detected.

### TDD steps

- [ ] Extract a pure classification helper that accepts existing-install presence, portable eligibility, and custom marker values.
- [ ] RED: no installation -> `None`.
- [ ] RED: installed SumatraPDF with no recognized custom marker -> `OfficialOrLegacy`.
- [ ] RED: installed SumatraPDF with matching `CustomDistributionId` -> `ManagedCustom`.
- [ ] RED: portable-only discovery -> `PortableOnly` and not eligible for managed in-place upgrade.
- [ ] RED: unknown/foreign custom marker -> do not classify as this managed custom distribution.
- [ ] Implement the minimum classification logic.
- [ ] Wire `GetPreviousInstallInfo()` to populate classification without changing files or registry during detection.
- [ ] Run unit tests, debug build, and `git diff --check`.

**Checkpoint evidence:** all four deployment states are deterministic and test-covered.

---

## Task 4: Persist and restore custom installation provenance

**Traceability:** Design 5.3, 5.7; DP-02, DP-08, DP-10  
**Commit:** `feat: persist custom installation provenance`

**Files:**
- Modify: `src/Installer.h`
- Modify: `src/Installer.cpp`
- Modify: `src/Uninstaller.cpp` only if the current uninstall-registration removal requires explicit custom-value cleanup.
- Add focused registry-value serialization/snapshot tests using pure data helpers; real registry tests remain Windows integration tests.

### Contract

Use the existing canonical SumatraPDF uninstall registration and add:

- `CustomDistributionId = sage1993.sumatrapdf_print`
- `CustomDistributionVersion = custom.1`
- `CustomUpstreamVersion = CURR_VERSION`

Do not create a second application/product uninstall root.

Before mutating install-registration values during an upgrade, capture the values this run may overwrite/delete so rollback can restore the exact prior state.

### TDD steps

- [ ] RED: test construction/parsing of recognized provenance values.
- [ ] RED: test snapshot semantics for missing, official/legacy, and prior custom values.
- [ ] Implement marker read helpers used by Task 3 classification.
- [ ] Extend `WriteUninstallerRegistryInfo()` or its narrow helper to write custom provenance only after application-file replacement is ready to commit.
- [ ] Add a small registration snapshot structure covering only values changed by this deployment operation.
- [ ] Add restore logic that recreates prior values and removes values that did not exist before the failed run.
- [ ] Ensure custom uninstall removes application registration through the existing canonical path without touching LocalAppData.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows integration: inspect Add/Remove Programs registration before/after official->custom, custom->custom, rollback, and uninstall.

**Checkpoint evidence:** provenance distinguishes managed Custom while canonical SumatraPDF registration remains singular.

---

## Task 5: Add installed-mode settings safety backup

**Traceability:** Design 5.5; DP-03, DP-04, DP-09  
**Commit:** `feat: preserve settings during custom upgrade`

**Files:**
- Modify: `src/Installer.h`
- Modify: `src/Installer.cpp`
- Reuse path/file primitives from `src/AppTools.cpp` and `src/base/File.*`; do not call the portable-sensitive installed-app resolver blindly.
- Add path-resolution and copy-policy unit tests.

### Contract

For the current interactive user, resolve the installed-mode LocalAppData SumatraPDF directory explicitly, independent of whether the installer executable itself is considered portable.

Source:

`<LocalAppData>\SumatraPDF\SumatraPDF-settings.txt`

Backup:

`<LocalAppData>\SumatraPDF\SumatraPDF-settings.pre-custom-install.txt`

One backup slot is replaced on each later upgrade. No source file means success/no-op. Copy failure is reported but cannot delete, truncate, rewrite, or relocate the source settings file.

### TDD steps

- [ ] Extract a pure helper that composes the installed-mode settings and backup paths from an injected LocalAppData base path.
- [ ] RED: assert paths remain under `SumatraPDF` even when the installer runs from an arbitrary/portable directory.
- [ ] RED: assert source and backup names exactly match the approved contract.
- [ ] Implement explicit installed-mode LocalAppData resolution.
- [ ] Adapt/reuse `CopySettingsFile` so the backup occurs before destructive application replacement for official->custom and custom->custom only.
- [ ] Make missing source a normal no-op.
- [ ] Make backup failure diagnostic/non-destructive; do not fall back to editing the live settings file.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows integration: seed preferences, favorites, history, window state; upgrade and compare effective post-upgrade state plus backup contents.

**Checkpoint evidence:** settings/favorites/history survive without schema migration.

---

## Task 6: Make running-process handling consent-first

**Traceability:** Design 5.6; DP-05  
**Commit:** `fix: require consent before closing SumatraPDF during upgrade`

**Files:**
- Modify: `src/Installer.cpp`
- Modify installer tests around process-conflict decision policy; OS Restart Manager behavior remains integration-tested.

### Contract

Current replacement paths that call `KillProcessesWithModule(..., true)` before confirmed user consent must be removed/reordered for managed upgrades.

Required decision sequence:

1. detect lock/process conflict without terminating it;
2. if no conflict, continue;
3. if conflict, ask for explicit consent;
4. cancel -> stop before destructive replacement;
5. consent -> invoke the existing close/termination mechanism;
6. still locked -> fail and roll back if destructive work already began.

### TDD steps

- [ ] Extract a pure decision helper for `NoConflict / NeedsConsent / Cancelled / MayTerminate / Abort` states.
- [ ] RED: cancellation never returns a state that permits destructive replacement.
- [ ] RED: termination cannot be selected before consent.
- [ ] Locate every `KillProcessesWithModule(..., true)` in the installer replacement flow and classify whether it is pre-consent.
- [ ] Refactor so lock detection and user confirmation occur before any forceful close/kill operation.
- [ ] Preserve the existing Restart Manager implementation after consent; do not add a second process-management subsystem.
- [ ] Ensure the installer returns a clear cancellation result without writing custom provenance or replacing files.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows integration: launch SumatraPDF, start upgrade, choose Cancel; hash/launch old executable and inspect registration to prove no partial upgrade.
- [ ] Windows integration: repeat with consent and verify successful replacement.

**Checkpoint evidence:** no silent pre-consent force termination remains.

---

## Task 7: Extend the existing `.copy` recovery mechanism into an upgrade transaction

**Traceability:** Design 5.7, 6.4; DP-10  
**Commit:** `feat: make custom upgrade rollback transactional`

**Files:**
- Modify: `src/Installer.h`
- Modify: `src/Installer.cpp`
- Add transaction-state and failure-injection unit/debug tests.

### Contract

Do not duplicate the existing `PrepareInstallDirByRenaming` / `.copy` restoration machinery. Wrap and extend it into an explicit transaction that tracks:

- whether destructive replacement has begun;
- staged/restorable application files;
- registration snapshot from Task 4;
- commit/rollback state;
- rollback diagnostic result.

User preferences are not part of rollback because deployment never modifies the live settings file.

### TDD steps

- [ ] Define a narrow `InstallUpgradeTransaction` (or equivalent) with explicit `Begin`, `Commit`, and `Rollback` ownership/lifetime semantics.
- [ ] RED: failure before destructive replacement performs no restore and leaves old state untouched.
- [ ] RED: injected failure after first destructive replacement requests application-file restore and registration restore exactly once.
- [ ] RED: successful commit removes/cleans staging and cannot later roll back from destructor/error cleanup.
- [ ] RED: rollback failure is surfaced as a distinct fatal result rather than being reported as install success.
- [ ] Adapt existing `.copy` helpers to the transaction instead of creating another file-backup format.
- [ ] Integrate Task 4 registration snapshot restore.
- [ ] Add deterministic failure-injection hooks enabled only in test/debug builds at representative post-replacement points.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows integration: inject mid-install failure, verify hashes/version/launch of restored previous executable and exact restoration of prior provenance values.

**Checkpoint evidence:** DP-10 is automated as far as pure state allows and has a reproducible Windows failure-injection acceptance path.

---

## Task 8: Integrate clean install, official->custom, and custom->custom flows

**Traceability:** Design 6.1-6.3; DP-01, DP-02, DP-03, DP-04, DP-06, DP-07, DP-08, DP-11  
**Commit:** `feat: integrate custom SumatraPDF in-place upgrade`

**Files:**
- Modify: `src/Installer.cpp`
- Modify: `src/Installer.h` only if final orchestration types need exposure.
- Modify association helpers only if required to preserve current behavior; do not add `UserChoice` writes.
- Add orchestration-policy tests.

### Contract

Flow ordering for an existing managed installation:

1. detect/classify previous installation;
2. preserve target install path/runtime identity;
3. backup current-user settings best-effort;
4. obtain process-close consent before destructive work;
5. begin rollback transaction;
6. replace files using existing robust installer path;
7. perform only existing legal registration/association refresh required for the installed SumatraPDF identity;
8. write custom provenance;
9. verify installed executable/custom identity;
10. commit transaction.

Clean install skips previous-state backup/rollback staging that has no source state.

Portable-only discovery must not silently convert the portable copy into a managed installed upgrade.

### TDD steps

- [ ] Add a pure orchestration decision table keyed by `PreviousInstallKind` and requested install mode.
- [ ] RED: `None` -> clean install.
- [ ] RED: `OfficialOrLegacy` -> managed in-place upgrade.
- [ ] RED: `ManagedCustom` -> managed in-place upgrade.
- [ ] RED: `PortableOnly` -> no managed in-place upgrade of that portable copy.
- [ ] RED: association policy never contains a `UserChoice` mutation operation.
- [ ] Wire Tasks 3-7 into the main installer sequence in the approved order.
- [ ] Confirm the same existing installation path is retained for official->custom unless the user explicitly selects a supported different install destination through existing installer policy.
- [ ] Ensure provenance is written only after successful app-file replacement and before final commit verification.
- [ ] Add post-install verification of executable presence and custom distribution identity.
- [ ] Run unit tests, debug build, and `git diff --check`.

**Checkpoint evidence:** all three managed lifecycle paths share one installer implementation with no parallel product identity.

---

## Task 9: Lock down uninstall preservation behavior

**Traceability:** Design 5.9, 6.5; DP-09  
**Commit:** `test: preserve SumatraPDF user state on uninstall`

**Files:**
- Modify: `src/Uninstaller.cpp` only if needed to make the preservation boundary explicit.
- Add regression tests around the uninstall deletion manifest/policy.

### Contract

Custom uninstall removes installed application files and canonical install registration. It must not delete:

- installed-mode `SumatraPDF-settings.txt`;
- favorites/history contained in the existing settings state;
- `SumatraPDF-settings.pre-custom-install.txt`.

It must not select a new Windows PDF default on the user's behalf.

### TDD steps

- [ ] Extract or expose a testable uninstall deletion policy/manifest if the current implementation is not directly testable.
- [ ] RED: assert installed-mode LocalAppData settings and backup paths are outside the deletion set.
- [ ] RED: assert uninstall policy includes no `UserChoice` mutation.
- [ ] Make the smallest implementation adjustment required; if existing code already satisfies the contract, keep production behavior unchanged and land regression tests only.
- [ ] Run unit tests, debug build, and `git diff --check`.
- [ ] Windows integration: install Custom, create state, uninstall, verify files/state remain, then reinstall official SumatraPDF and verify compatible state is reusable.

**Checkpoint evidence:** uninstall preservation is enforced by regression coverage rather than assumption.

---

## Task 10: Add deployment regression harness and installer artifact checks

**Traceability:** all automated portions of DP-01..DP-12  
**Commit:** `test: add custom deployment regression coverage`

**Files:**
- Add a focused Windows deployment test/helper script under the repository's existing `cmd/` or test-script convention.
- Modify build/test registration only as needed.
- Add test fixtures only for synthetic settings/registry data; do not commit user-specific registry exports or settings.

### Required automated checks

- [ ] Distribution identity constants/resource metadata.
- [ ] Official updater hard gates.
- [ ] Previous installation classification.
- [ ] Provenance read/write/snapshot/restore helper semantics.
- [ ] Installed-mode settings backup path.
- [ ] Consent-first process decision policy.
- [ ] Transaction failure-injection state machine.
- [ ] Orchestration decision table.
- [ ] Uninstall preservation policy.
- [ ] `git diff --check`.
- [ ] `bun cmd/run-unit-tests.ts -dbg`.
- [ ] `bun cmd/build.ts -debug`.
- [ ] Release installer artifact exists and VERSIONINFO reports Custom display metadata while the executable remains `SumatraPDF.exe`.

The harness must fail closed: a skipped destructive/registry test is reported as `NOT_RUN` or `SKIP` with a reason, not as PASS.

---

## Task 11: Execute Windows acceptance matrix and release gate

**Traceability:** DP-01 through DP-14  
**Commit:** documentation/evidence commit only after the matrix has actually been executed.

Create a dated acceptance record under `docs/superpowers/evidence/` or the repository's existing evidence convention. Record exact installer/executable hashes, Windows version, prior SumatraPDF version/custom version, and result for every gate.

### DP matrix

- [ ] **DP-01 Clean install:** no prior SumatraPDF; install and launch Custom.
- [ ] **DP-02 Official->Custom:** install official SumatraPDF, then run Custom installer without manual uninstall; same managed identity/path remains usable.
- [ ] **DP-03 Settings:** seed non-default preferences and verify they remain effective after upgrade.
- [ ] **DP-04 Favorites/history:** seed favorites/history and verify post-upgrade preservation.
- [ ] **DP-05 Running process:** with SumatraPDF running, Cancel leaves old app and registration unchanged; consent permits upgrade.
- [ ] **DP-06 SumatraPDF current PDF viewer:** PDF open remains functional after upgrade.
- [ ] **DP-07 Acrobat/other current PDF viewer:** default remains the other viewer; verify no takeover.
- [ ] **DP-08 Custom->Custom:** upgrade an older managed custom marker/version in place.
- [ ] **DP-09 Uninstall:** remove Custom and verify user state plus pre-custom backup remain.
- [ ] **DP-10 Failure rollback:** inject failure after destructive replacement and prove prior executable and changed registration are restored.
- [ ] **DP-11 Reboot:** reboot after successful install and verify launch/PDF-open behavior.
- [ ] **DP-12 Update isolation:** no Help update command; scheduled/manual/API paths cannot reach official metadata/download/installer.
- [ ] **DP-13 Print Preview:** run existing Custom Print Preview regression and human visual acceptance on the release build.
- [ ] **DP-14 ApeosPort:** compare physical ApeosPort output with accepted preview/print behavior.
- [ ] **Native/System Print gate:** open, cancel, re-enter, and print/fallback without stale state or crash.

### Release verdict

Release only if all applicable automated checks pass and all human/device gates are PASS. `CONDITIONAL PASS` is permitted for an internal development checkpoint but not for a distributable release artifact if DP-13, DP-14, Native/System Print, rollback, or default-app preservation remain unexecuted.

---

## Task-to-Requirement Traceability

| Requirement | Primary task(s) |
| --- | --- |
| Runtime identity preservation | 1, 8 |
| Custom product/version identity | 1, 4 |
| Official updater isolation | 2, 10, 11 |
| Existing install classification | 3 |
| Provenance marker | 4 |
| Settings/favorites/history preservation | 5, 9, 11 |
| Consent before process termination | 6, 11 |
| Application/registration rollback | 4, 7, 11 |
| Official->Custom in-place upgrade | 8, 11 |
| Custom->Custom upgrade | 8, 11 |
| Portable exclusion | 3, 8 |
| Default-app/UserChoice preservation | 8, 9, 11 |
| Uninstall preservation | 9, 11 |
| Automated regression | 10 |
| Print Preview / Native Print / ApeosPort acceptance | 11 |

## Commit/Push Policy

- One implementation task per checkpoint commit unless a task proves to be test-only and inseparable from the immediately preceding task.
- Never mix unrelated Print Preview feature work with deployment commits.
- Before each commit: relevant RED evidence exists, GREEN unit/build evidence is recorded, and `git diff --check` passes.
- Do not mark Windows integration/manual gates PASS without actual execution evidence.
- Push checkpoint commits to `origin/feat/custom-print-preview` only after local Windows verification for that checkpoint, unless the user explicitly requests remote backup of an unverified work-in-progress state.
- Do not open or merge a release PR while any release-blocking DP gate is `FAIL`, `NOT_RUN`, or unresolved.
