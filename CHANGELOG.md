# Changelog

All notable changes to Dzjs Trainer are documented in this file.

## [Unreleased]

### Changed

- The update flow now lives entirely in the standalone `DzjsTrainerUpdater.exe`. The main program no longer downloads, verifies, or applies anything; the update entry on the About page extracts the embedded updater next to the main program and starts it.
- The updater checks the published version itself, downloads the package, verifies its SHA-256, and then asks the user to close the main program before replacing it. It polls until the file is no longer locked instead of waiting on a parent process id.
- The updater now runs from the install directory instead of `%TEMP%`, and requests elevation itself so a declined prompt is reported instead of being treated as success.
- Certificate validation is enabled on the update channel.

### Fixed

- The download URL from the manifest is decoded as UTF-8 and its host is converted to punycode before use. Decoding it with the ANSI code page corrupted the host, so every download failed with a connection error.
- `CompareVersions` treats a missing component as zero, so `1.0.5` and `1.0.5.0` no longer compare as different.
- The updater's version comparison reads the installed program's file version rather than the updater's own build-time constant.

### Removed

- The in-app download window, its progress reporting, and the payload/parent-pid helper path.
- The `AutoUpdate` setting and the startup update timer. The new flow requires the user to close the main program, so an unattended check cannot work.

## [1.0.2] - 2026-09-06

### Added

- Added a dedicated homepage/header action for super topmost.
- Added runtime UIAccess relaunch and token verification based on the `D:\\原桌面\\class` implementation.

### Changed

- The super-topmost header button now displays `超级置顶：已开启` or `超级置顶：未开启` from the effective UIAccess and topmost window state.
- Updated the application version to `1.0.2` and Windows file/product version to `1.0.2.0`.

### Release Package

- `DzjsTrainer-v1.0.2-win32.zip` contains the Win32 main executable only.
- The package includes the embedded signed primary driver.
- A SHA-256 checksum file is published alongside the archive.

## [1.0.1] - 2026-09-05

### Fixed

- Corrected the confirmation private-desktop cleanup sequence. The application now returns to the previous desktop before closing the confirmation desktop, and retains that desktop when restoration does not succeed.
- Removed an unnecessary foreground-window transition across desktops and guarded edit-control subclass cleanup against an already-destroyed window.
- Added timeout and exception handling around background repository cleanup so an unresponsive cleanup operation cannot block the application indefinitely.
- Changed a transient driver heartbeat communication failure from a process-termination path to a logged degraded state; driver-backed operations can retry on their next use.
- Hardened application-boundary exception handling and crash-report path construction.

### Changed

- Updated the application version to `1.0.1` and Windows file/product version to `1.0.1.0`.

### Release Package

- `DzjsTrainer-v1.0.1-win32.zip` contains the Win32 main executable only.
- The existing signed embedded primary driver is unchanged from the previous release.
- A SHA-256 checksum file is published alongside the archive.

## [1.0.0] - 2026-08-30

- Initial public release of Dzjs Trainer.
