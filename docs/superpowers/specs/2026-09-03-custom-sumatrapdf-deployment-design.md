# Custom SumatraPDF Deployment and Upgrade Design

**Date:** 2026-09-03  
**Repository:** `sage1993/sumatrapdf_print`  
**Branch:** `feat/custom-print-preview`  
**Status:** Design approved in chat; implementation not started

## 1. Context

The custom Print Preview work changes SumatraPDF application behavior, but existing users may already have the official SumatraPDF installed, registered for PDF files, and configured with user preferences, favorites, history, window state, and printer-related preferences.

The deployment objective is therefore not a second unrelated viewer installation. The custom build must behave as an in-place managed upgrade of an existing installed SumatraPDF while preserving the runtime identity that current settings and file associations depend on.

The existing repository already contains substantial installer, uninstaller, file-association, settings, and update-check infrastructure. This design extends those paths rather than introducing a second installer subsystem.

## 2. Goals

1. Support clean installation of the custom build.
2. Detect an existing installed SumatraPDF and upgrade it without requiring the user to uninstall it manually.
3. Distinguish an official/legacy installation from a managed custom installation.
4. Preserve existing user settings, favorites, history, and UI state.
5. Preserve the existing SumatraPDF executable and runtime identity so current paths and associations continue to work.
6. Prevent the official SumatraPDF update mechanism from replacing the custom build.
7. Preserve the user's current Windows default PDF application; never seize `.pdf` from Acrobat or another viewer.
8. Support custom-to-custom upgrades.
9. Preserve user state when uninstalling the custom build.
10. Restore the previous runnable application files if an upgrade fails after destructive file replacement has begun.
11. Make the custom build clearly identifiable through version/resource/About information.
12. Require real upgrade, rollback, Native/System Print, Print Preview, and ApeosPort acceptance tests before release.

## 3. Non-goals

The first deployment release does not include:

- a custom automatic-update service;
- background enterprise software distribution;
- modification of Windows `UserChoice` default-app records;
- automatic takeover of `.pdf` from another viewer;
- migration of every Windows user's LocalAppData profile on a multi-user machine;
- managed in-place conversion of portable SumatraPDF copies;
- renaming the executable from `SumatraPDF.exe`;
- changing the global runtime application identity from `SumatraPDF` to a new application name;
- system snapshots or MSI-style full-machine transactional rollback.

Portable copies remain independent. The managed-upgrade path applies to an installed SumatraPDF detected through the existing installation/registry logic.

## 4. Verified Existing Implementation Touchpoints

| File | Verified current behavior | Deployment implication |
| --- | --- | --- |
| `src/Installer.h` | Defines `PreviousInstallationInfo`; exposes existing installation detection, association re-registration, uninstall-registry and search-filter helpers. | Extend the current install classification rather than create a parallel detector. |
| `src/Installer.cpp` | Copies installer payload into the selected install directory and contains robust locked-file handling using Windows Restart Manager and user confirmation. | Reuse the existing replacement path. Silent force termination remains prohibited. Add rollback staging around destructive replacement. |
| `src/Uninstaller.cpp` | Removes application/install registrations and installed program files. No installed-mode LocalAppData settings deletion was found in the inspected path. | Preserve this separation and add regression coverage proving user state survives uninstall. |
| `src/AppSettings.cpp` | Uses `SumatraPDF-settings.txt`; settings include state such as history/favorites and are loaded/saved through existing settings logic. | Do not migrate or rewrite the live settings file during deployment. Create only a safety backup. |
| `src/AppTools.cpp` | Installed-mode app data resolves from LocalAppData plus `kAppName`; portable mode uses the executable directory. | Keep `kAppName` unchanged. Installer backup logic must explicitly resolve the installed-mode LocalAppData location rather than inheriting the installer's own portable-mode context. |
| `src/UpdateCheck.cpp` | Uses official `sumatrapdfreader.org` update metadata and official release installers; implements scheduled/user-initiated checks and installer auto-upgrade. | Custom distributions must hard-disable both update checks and official auto-upgrade calls. |
| `src/UpdateCheck.h` | Exposes update-check/upgrade entry points used by the application. | Apply the custom-distribution guard at API entry points, not only in preferences/UI. |
| `src/Menu.cpp` | Help menu contains `Check for Updates` command wiring. | Hide the command for the custom distribution so the UI matches runtime policy. |
| `src/Version.h` | Defines `CURR_VERSION`, `CURR_VERSION_COMMA`, `kAppName "SumatraPDF"`, and resource-version macros. | Preserve `kAppName`; introduce separate custom distribution/resource identity. |
| `src/SumatraPDF.rc` | VERSIONINFO currently derives `FileDescription` and `ProductName` from `kAppName`, and version strings from existing resource macros. | Use separate resource display macros for the custom product while retaining numeric upstream-compatible version resources where required. |

## 5. Architecture Decisions

### 5.1 Preserve Runtime Identity

`kAppName` remains:

```cpp
#define kAppName "SumatraPDF"
```

The executable also remains `SumatraPDF.exe`.

This is a compatibility invariant. Existing installed-mode user data is derived from LocalAppData plus `kAppName`, and existing registry/file-association behavior also depends on the current identity. Globally renaming `kAppName` would create a second settings/registry namespace and would defeat transparent upgrade behavior.

### 5.2 Separate Distribution Identity from Runtime Identity

The custom distribution receives a separate display/provenance identity without changing `kAppName`.

Required logical values are:

- distribution product name: `SumatraPDF Custom`;
- distribution identifier: `sage1993.sumatrapdf_print`;
- custom release line: `custom.1`, incremented for later internal releases;
- upstream base version: the repository's `CURR_VERSION` / `CURR_VERSION_COMMA` values;
- executable name: unchanged `SumatraPDF.exe`.

For VERSIONINFO, numeric `FILEVERSION` / `PRODUCTVERSION` remain based on `CURR_VERSION_COMMA`. String metadata identifies the custom build, for example `3.7-custom.1` for the current `CURR_VERSION 3.7` base. `FileDescription` and `ProductName` display `SumatraPDF Custom`.

The About UI must expose at least:

- `SumatraPDF Custom`;
- custom distribution version;
- upstream base version;
- build date.

### 5.3 Installation Provenance Marker

The existing SumatraPDF uninstall registration remains the canonical install registration. The custom installer adds provenance values to that existing registration rather than creating a second product root.

Required values:

- `CustomDistributionId` = `sage1993.sumatrapdf_print`;
- `CustomDistributionVersion` = current custom release, initially `custom.1`;
- `CustomUpstreamVersion` = current upstream base version.

Classification rules:

- existing installed SumatraPDF + no recognized custom marker -> official/legacy installation;
- existing installed SumatraPDF + matching `CustomDistributionId` -> managed custom installation;
- portable copy -> not eligible for managed in-place upgrade;
- no existing installation -> clean install.

`PreviousInstallationInfo` or an adjacent deployment classification structure will carry this provenance into installer decisions.

### 5.4 Official Update Mechanism Is Hard-disabled

For the managed custom distribution, disabling the preference alone is insufficient. The official update path must be blocked at three levels:

1. the Help menu does not expose `Check for Updates`;
2. update-check eligibility rejects the custom distribution regardless of saved `checkForUpdates` preference or user-initiated status;
3. the official installer auto-upgrade entry point rejects execution for the custom distribution.

The custom build must not contact the official update metadata endpoint as part of update checking and must not download/launch an official SumatraPDF installer through the existing auto-upgrade path.

The existing preference may remain in the settings schema for compatibility, but it has no effect on the custom distribution.

There is no replacement custom updater in deployment v1.

### 5.5 Settings and User-state Preservation

The installer must not delete, relocate, parse-and-rewrite, or migrate the live user settings as part of a normal upgrade.

For the current interactive user in installed mode, the default state location remains the existing application location under LocalAppData with `kAppName == "SumatraPDF"`, including:

`SumatraPDF-settings.txt`

Before replacing application files during an official-to-custom or custom-to-custom upgrade, the installer makes a best-effort copy of the current settings file to:

`SumatraPDF-settings.pre-custom-install.txt`

in the same installed-mode SumatraPDF LocalAppData directory.

The backup is replaced on each later upgrade; deployment v1 does not accumulate an unbounded backup history.

Important resolver rule: installer backup code must explicitly resolve the installed-mode LocalAppData SumatraPDF directory. It must not blindly call a path helper whose behavior can switch to the installer's own executable directory when the installer process is classified as portable.

Backup failure is reported but does not justify deleting or rewriting the live settings file. If the source settings file does not exist, installation continues normally.

Multi-user profile backup is outside v1 scope; other Windows users' LocalAppData is not enumerated or modified.

### 5.6 Running-process Handling

Silent force termination is prohibited.

When target files are locked by running SumatraPDF processes, reuse the current Restart Manager workflow subject to these rules:

1. detect the conflict;
2. present explicit user confirmation before closing/terminating the conflicting application;
3. cancellation exits the upgrade without destructive continuation;
4. after consent, the existing confirmed close/termination path may proceed;
5. if safe replacement still cannot continue, fail the installation and invoke application-file rollback if replacement has begun.

### 5.7 Application-file Rollback Transaction

DP-10 requires more than settings preservation: if installation fails after replacing part of the application, the previous version must remain runnable.

The installer therefore introduces a bounded application-file transaction:

1. determine the set of installed application files that will be replaced or removed;
2. before the first destructive replacement, copy the prior versions of those files into a per-install rollback staging directory under the system/user temporary directory;
3. perform installation using the existing robust copy/replacement path;
4. on success, remove rollback staging;
5. on failure after destructive work begins, restore the staged files and restore the prior install-registration values that the current run changed;
6. report rollback success or failure explicitly.

The transaction covers custom installer application files and registration values changed by the deployment operation. It does not snapshot unrelated system state and does not roll back user preferences because the installer never modifies the live preferences in the first place.

### 5.8 File-association Policy

The deployment reuses the existing SumatraPDF registration and association helpers, including the existing registration identity and executable path. It does not manipulate Windows `UserChoice`.

Required behavior:

- if SumatraPDF was already the effective/default PDF viewer and the same installed application path remains valid, the custom upgrade must remain usable for PDF open operations;
- if Acrobat or another viewer is the default, the custom installer does not take over `.pdf`;
- existing non-default SumatraPDF registrations may be re-registered through existing helper logic as needed;
- file association behavior must be verified on the target Windows version rather than assumed from registry writes alone.

### 5.9 Uninstall and Return to Official SumatraPDF

Custom uninstall removes custom application files and the custom install/provenance registration but does not remove the user's SumatraPDF LocalAppData settings, favorites, or history.

A user or administrator may then install the official SumatraPDF again. Because runtime identity and settings namespace were preserved, the official build can reuse compatible existing settings subject to upstream version compatibility.

The custom uninstaller does not automatically download or reinstall the official build.

## 6. Lifecycle Flows

### 6.1 Clean Install

1. No managed installed SumatraPDF is detected.
2. Installer selects the normal install destination using existing installer policy.
3. No settings migration is performed.
4. Custom application files and registration are installed.
5. Custom provenance values are written.
6. Existing Windows default PDF application is not forcibly changed.
7. Installed executable/version metadata is verified.

### 6.2 Official SumatraPDF to Custom

1. Detect existing install and classify it as official/legacy because the custom marker is absent.
2. Preserve the same runtime identity and managed install path.
3. Best-effort backup the current interactive user's installed-mode settings file.
4. Detect running/locking processes and require consent before any close/termination path.
5. Stage application files and affected registration values for rollback.
6. Replace application files using existing robust installer logic.
7. Re-register only the associations required by the existing installer flow; do not change `UserChoice`.
8. Write custom provenance/version metadata.
9. Verify executable launch and custom identity.
10. Remove rollback staging only after installation success.

### 6.3 Custom to Custom

The same upgrade pipeline is used, except the existing provenance marker identifies a managed custom version. The installer records the previous custom version for diagnostics, creates the latest single settings safety backup, stages prior application files, and upgrades in place.

### 6.4 Installation Failure

If failure occurs before destructive replacement, abort without changing the existing installation.

If failure occurs after destructive replacement begins:

1. stop further installation work;
2. restore staged prior application files;
3. restore prior registration values changed by this run;
4. leave the live settings/user state untouched;
5. verify that the previous executable exists and can be launched or, where launch cannot be automated safely, report that executable restoration succeeded and require the defined manual launch gate;
6. retain sufficient installer diagnostics to identify the failed step.

A rollback failure is a release-blocking defect.

### 6.5 Uninstall

1. Unregister/remove application-specific install registrations using the current uninstaller flow.
2. Remove installed custom application files.
3. Do not delete LocalAppData `SumatraPDF-settings.txt`, favorites/history state, or the pre-custom settings backup.
4. Do not silently change the user's default PDF application to another program.

## 7. Error Handling and Invariants

The deployment implementation must maintain these invariants:

- `SumatraPDF.exe` and `kAppName == "SumatraPDF"` remain stable.
- User state is never a prerequisite for destructive migration because there is no state migration in v1.
- Failure to make the optional safety backup never causes deletion of the live settings file.
- A user cancellation at a running-process prompt does not partially upgrade the application.
- An installation failure after replacement begins triggers rollback.
- Official update checks and official auto-upgrade cannot execute in the custom distribution.
- The installer never writes Windows `UserChoice` to seize `.pdf`.
- Portable SumatraPDF is not treated as a managed installed copy.
- Uninstall does not remove installed-mode user preferences.
- Custom display identity is separate from runtime/storage identity.

## 8. Verification Matrix

| ID | Scenario | Required result |
| --- | --- | --- |
| DP-01 | SumatraPDF not installed | Custom clean install succeeds and launches. |
| DP-02 | Official installed SumatraPDF | In-place custom upgrade succeeds without manual uninstall. |
| DP-03 | Existing user settings | Settings remain effective after upgrade. |
| DP-04 | Existing favorites/history | Favorites/history remain available after upgrade. |
| DP-05 | `SumatraPDF.exe` running/locking files | User receives explicit close/termination confirmation; cancellation leaves old install intact. |
| DP-06 | SumatraPDF is current PDF viewer | PDF open behavior remains functional after upgrade. |
| DP-07 | Acrobat/other viewer is current default | Default remains the other viewer; custom installer does not seize `.pdf`. |
| DP-08 | Older managed custom build | Custom-to-custom in-place upgrade succeeds. |
| DP-09 | Custom uninstall | Application is removed while user preferences remain. |
| DP-10 | Injected mid-install failure | Prior application files/changed registration are restored and prior version remains runnable. |
| DP-11 | Reboot after successful install | PDF open and application launch remain functional. |
| DP-12 | Update command/scheduled check paths | Custom build performs no official update check or official auto-upgrade. |
| DP-13 | Custom Print Preview | Existing Print Preview regression suite and human visual checks pass on release build. |
| DP-14 | Physical ApeosPort output | Printed output matches accepted preview/print behavior. |

Additional automated coverage must include focused tests for distribution classification, custom marker read/write, update hard-gates, installed-mode backup path resolution, rollback success/failure paths, and preservation of association policy helpers.

## 9. Release Gate

A distributable custom installer is releasable only when all applicable gates below pass:

- Debug/release build: PASS
- Unit/regression tests: PASS
- Print Preview regression: PASS
- Native/System Print regression: PASS
- DP-01 clean install: PASS
- DP-02 official-to-custom upgrade: PASS
- DP-03/DP-04 user-state preservation: PASS
- DP-05 running-process cancellation/consent: PASS
- DP-06/DP-07 file-association behavior: PASS
- DP-08 custom-to-custom upgrade: PASS
- DP-09 uninstall preservation: PASS
- DP-10 injected failure rollback: PASS
- DP-11 reboot/open behavior: PASS
- DP-12 official update isolation: PASS
- DP-13 Print Preview acceptance: PASS
- DP-14 physical ApeosPort QA: PASS

Any failure in upgrade safety, rollback, settings preservation, file-association non-takeover, official-update isolation, Native/System Print, Print Preview, or physical printing blocks internal release.

## 10. Implementation Workstreams

### D1. Baseline Deployment Inspection

Lock down current installer/uninstaller behavior, registry values, build outputs, and existing tests before modification. Capture official-install and current-custom baselines.

### D2. Custom Distribution Identity

Add the separate custom distribution/resource identity while preserving `kAppName` and `SumatraPDF.exe`. Add provenance values to the existing uninstall registration and expose custom/upstream/build information in the existing About UI.

### D3. Official Update Isolation

Hide the Help update command for custom builds and add runtime hard-gates to update-check and official auto-upgrade entry points. Add tests proving saved preferences/user-initiated paths cannot bypass the gate.

### D4. Existing-install Classification

Extend `PreviousInstallationInfo` or a narrowly-scoped adjacent type to classify clean, official/legacy installed, managed custom installed, and non-managed portable states.

### D5. User-state Safety Backup

Implement installed-mode LocalAppData resolution for deployment and the single pre-custom settings backup without changing the live state file.

### D6. Transactional Upgrade

Wrap existing robust installer file replacement and affected registration writes with rollback staging/restoration. Preserve explicit process-close consent.

### D7. File-association Regression

Verify/reuse existing association helpers and prove the installer does not manipulate `UserChoice` or take over from another current default viewer.

### D8. Uninstall and Rollback Verification

Prove LocalAppData user state survives uninstall and injected installation failures restore the prior runnable application state.

### D9. Real Official-to-Custom Acceptance

Run on a representative Windows environment with an actual official SumatraPDF installation containing real preferences/favorites/history. Execute DP-02 through DP-12, including reboot where specified.

### D10. Internal Release Package

Build the final installer/package from the accepted commit, record custom/upstream version and commit SHA, execute Print Preview/Native Print/ApeosPort release gates, and archive the acceptance record with the release artifact.

## 11. Compatibility Constraints

The implementation plan must preserve these explicit decisions:

1. Existing users do not manually uninstall official SumatraPDF before installing the custom build.
2. `SumatraPDF.exe` remains the executable name.
3. `kAppName` remains `SumatraPDF`.
4. Product/resource/About identity, not runtime storage identity, distinguishes the custom distribution.
5. The existing uninstall registration remains canonical and gains custom provenance values.
6. Installed-mode user data remains in the existing LocalAppData SumatraPDF namespace.
7. The installer creates only one latest pre-custom settings backup for the current interactive user.
8. Official update checking and official installer auto-upgrade are disabled by runtime policy, not merely by default preference.
9. No custom updater is introduced in v1.
10. Windows `UserChoice` is never manipulated by this deployment feature.
11. Portable copies are not managed-upgrade targets.
12. Silent force-killing of running SumatraPDF is prohibited; current confirmation-driven Restart Manager handling may be reused.
13. Application-file rollback is required for failures after destructive replacement begins.
14. Uninstall preserves user settings/state.
15. Physical ApeosPort and Native/System Print validation remain release gates, not optional post-release checks.

## 12. Design Completion Criteria

This design is ready for implementation planning when the repository copy has been reviewed and the user explicitly approves implementation planning. Production-code changes must not start before that approval.