# Changelog

All notable changes to Dzjs Trainer are documented in this file.

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
