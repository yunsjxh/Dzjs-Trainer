# Changelog

All notable changes to Dzjs Trainer are documented in this file.

## [1.0.6] - 2026-11-21

### Added

- The about page has a feedback card: what went wrong, how to reproduce it, and an optional way to
  reach you. Submitting it packs the day's log and a system snapshot and uploads them, signed, so
  the server only accepts requests that came from a real client.
- Several classroom products can be handled at the same time. JiYu, Red Spider and Gaga Card are
  tracked on their own, and the overview card names every product that was adopted.
- Ending a classroom asks which product to end, or all of them, and removes the parent service and
  the install folder on the way out.
- The protection status on the overview page opens a read-only panel: overall state, controlled
  hosts, adopted classrooms, driver, self-protection, blocked items, version and build time.
- Shutdown, restart and logoff requests coming from a classroom are blocked, in the sending process
  as well as in the trainer. `BlockShutdown=FALSE` in the ini turns it off.

### Changed

- Logs are written one file per day under `logs/`.

### Fixed

- A classroom that is still running is no longer reported as dead, and the status lines no longer
  overlap.
- The classroom menu is drawn by the trainer instead of being requested from the system.
- The build time on the about page is read from the PE header, so it shows the minute the file was
  linked even on an incremental build.

## [1.0.5.3] - 2026-11-21

### Added

- The main window is per-monitor DPI aware and the secondary dialogs scale with the monitor they
  open on, so at 125% and 150% nothing comes up smaller than the rest of the desktop.

### Fixed

- Esc and Enter work in the process picker, the sample picker and the log save picker; Tab moves
  focus in the AV text prompt.
- The tray icon repair runs at all: it shared a timer id with the rerun check and the rerun branch
  always won.
- Selecting a student in the teacher pane switches the live view right away instead of waiting for
  the next list refresh.
- Typing in the teacher chat box no longer dispatches every keystroke as a button command and
  fills the result pane.
- Double-clicking a folder in the media picker enters it; double-clicking "up" no longer goes up
  two levels.
- Embedding the teacher GUI no longer terminates the process when the window search times out; it
  falls back to the standalone window.
- A fresh clone builds without a hardcoded Boost include path or the missing injector project.

## [1.0.5.2] - 2026-11-15

### Added

- The about page and the startup log show the run environment: Windows version with its patch
  number, native architecture, elevation, UI access and session type.

### Changed

- Screen captures and desktop sharing see through the trainer window instead of getting a black
  rectangle. `[JTSettings] CaptureExclusion` in the ini picks the behaviour: 1 see-through
  (default), 2 black block, 0 visible as usual. Systems older than Windows 10 2004 fall back to
  visible, because there the see-through flag renders as a black block.

### Fixed

- The sidebar hover no longer flashes brighter before settling darker in the light theme; both
  ends of the hover gradient now sit on the same side of the sidebar colour.

## [1.0.5.1] - 2026-11-15

### Fixed

- The first "check for updates" click no longer looks dead: the apex host is tried first, each
  request allows 6 seconds instead of 8, and the bundled updater is reused instead of rewritten,
  so no full antivirus scan runs on every click.
- The toast at the bottom of the window follows the theme. It was drawing the light palette in
  dark mode, which is why it always came out near-white.

### Added

- The about page and the update-probe log line show the build date, read from the PE header
  timestamp so incremental builds do not report a stale day.

## [1.0.5] - 2026-11-15

### Added

- The protection page shows a driver-status chip next to "卸载主驱动": whether this process holds
  the device handle, whether the service exists and is running, and its name. The sample is cached
  for 1.5 seconds and invalidated on load/unload.
- The exit dialog's verification code is valid for 30 seconds. When it expires a new six-digit code
  is drawn, whatever has been typed is cleared, and a `27 秒后刷新` countdown sits at the right end
  of the code row. Only the countdown rectangle repaints each second.
- Four hooks close coverage gaps: `PrintWindow`, `GetDIBits` and `StretchBlt` in the display domain,
  and `SetWindowsHookExW` in the input domain. They reuse `AllowMonitor` and `AllowControl`.
- Image replacement runs on the generic GDI hooks instead of only inside JiYu's own encoder, so
  `StretchBlt`, `PrintWindow` and `GetDIBits` all return the chosen image. `GetDIBits` paints
  24- and 32-bpp buffers and keeps the solid fill for 8/16-bpp.

### Changed

- Both keys behind driver unload authorization were rotated: the ECDSA P-256 pair that signs the
  token and the AES-256 key that wraps it. Old tokens are rejected by the new driver.
- Image replacement is no longer confined to JiYu's own processes. Inject any program and its screen
  captures come back as the chosen image. `BitBlt` still requires a `SRCCOPY` from a desktop DC into
  a memory DC, so a host's own drawing is untouched. `[CustomInject] Mode=minimal` still skips the
  display domain, and replacement does not work in that mode.
- The blurred desktop behind the exit dialog was rendering as a flat dark green field; all three
  tinted layers are now neutral dark grey, and the blur is tightened to 320×180 at radius 3.
- The exit dialog's full-screen surface no longer carries `WS_EX_DLGMODALFRAME` or `WS_BORDER`,
  which drew a 1px system frame around the dimmed wallpaper.
- The exit dialog's code field sits inside a self-drawn 1px rounded outline instead of the native
  3D sunken border, and its fill matches the panel.
- The exit dialog's inner gaps are a uniform 16px; the error line stays tight against the hint.
- The exit dialog's two buttons are registered for hover tracking, and the dialog draws with
  antialiased geometry and text.
- The exit dialog's panel geometry is a single block of constants shared by `PaintExitConfirm` and
  `WM_CREATE`.
- The wallpaper blur is separable, one horizontal pass then a vertical one, cutting the sample count
  from 2.62M to 1.04M.
- The update flow lives entirely in the standalone `DzjsTrainerUpdater.exe`; the About page extracts
  the embedded updater and starts it.
- The updater checks the published version, downloads the package, verifies its SHA-256, and waits
  for the main program to close before replacing it.
- The updater runs from the install directory instead of `%TEMP%` and requests elevation itself, so
  a declined prompt is reported instead of being treated as success.
- Certificate validation is enabled on the update channel.
- Every hook is record-only by default: a detour with its switch off writes one `RECORD` line and
  calls the original. Two switches are new, "拦截远程窗口操作" (`BlockWindowOps`) and
  "拦截极域驱动访问" (`BlockDriverAccess`). Existing installations are migrated once with
  `EnforceMode=record`.
- The window theme follows the Windows app theme, title bar included, and repaints on
  `WM_SETTINGCHANGE`. Clicking the theme button switches to a manual choice for the rest of the
  session.

### Fixed

- The driver signature policy rejected the program's own embedded driver. The pin still carried the
  old test certificate (`JiYuTrainerDriver-Yunsjxh`) while the release driver was signed with
  `DzjsTrainerDriver By:Yunsjxh`. The pin now holds the current subject and thumbprint, and the
  decision checks the trust status before consulting it.
- `Build-DzjsTrainerRelease.ps1` derived the pin with `Subject -replace '^CN='`, which wrote the
  whole distinguished name into the pin. It now uses `GetNameInfo(SimpleName)`, the same call the
  runtime makes.
- `GuardTerminator` waited the full 60 seconds for a guard process that was not installed, then
  logged an error every cycle. It returns immediately when nothing is found, and a guard that ends
  without being respawned is a warning.
- Unloading custom injection at exit opened processes that had already terminated and reported
  `20002` / `0xC000000B` for each. Gone targets are now skipped.
- The process picker's search field could not be typed into under a Chinese input method and showed
  no caret. It is now a real `EDIT` child window with a cue banner, so focus, caret, selection, the
  clipboard, the context menu and IME composition all come from the control. `EN_CHANGE` feeds the
  filter, and a subclass forwards the keys the list owns plus the wheel. `Tab` now moves the focus
  ring instead of being swallowed.
- `CreateDCW(L"DISPLAY")` was a hole in the capture filter: `WindowFromDC` returns no window for a
  device DC, so the path was waved through as a memory DC. The filter now distinguishes window DCs
  from display DCs and requires a memory-DC destination for the latter; the `GetDIBits` branch
  additionally requires a screen-sized bitmap.
- 16-bpp `GetDIBits` targets were filled solid black instead of getting the replacement image. It
  now packs 555 for `BI_RGB` and 565 for `BI_BITFIELDS`, and still falls back to the solid fill for
  other masks.
- Owner-drawn buttons showed a pale halo around their rounded corners, because `WM_CTLCOLORBTN` fell
  through to the default `COLOR_BTNFACE`. `DrawNativeButton` now fills the whole button rectangle
  with the surface colour, and both window procedures answer `WM_CTLCOLORBTN`.
- The exit dialog flashed a black frame on each countdown tick. The whole surface is now rendered
  into an `ExitBackBuffer` and handed to the window in a single `BitBlt`, clipped to `ps.rcPaint`.
- The download URL from the manifest is decoded as UTF-8 and its host converted to punycode;
  decoding it with the ANSI code page corrupted the host.
- `CompareVersions` treats a missing component as zero, so `1.0.5` and `1.0.5.0` no longer compare
  as different.
- The updater's version comparison reads the installed program's file version rather than its own
  build-time constant.

### Removed

- The in-app download window, its progress reporting, and the payload/parent-pid helper path.
- The `AutoUpdate` setting and the startup update timer. The new flow requires the user to close the
  main program, so an unattended check cannot work.
- The "使用帮助" page is gone from the sidebar; its slot is taken by "进程注入", which moves up from
  the last position. The tray menu's "帮助" item shows the same short help message the
  broadcast-window menu already used.

## [1.0.4.3] - 2026-11-14

### Added

- Custom process injection: the new "进程注入" page injects the hooks DLL into a user-specified PID
  or process name, for both 32-bit and 64-bit targets.
- The injection page picks targets from a built-in process list showing icon, PID, image path,
  bitness and whether the process is already injected, with type-to-filter and keyboard navigation.
- The list is multi-select: click toggles a row, Shift+click selects a range, Ctrl+A selects
  everything the filter shows. The action button shows the count and reports failures by name.
- System-critical processes can be injected like any other target and are only tagged "系统"; the
  trainer's own process stays non-selectable.
- CLI switches `-inject <PID|name>` and `-uninject <PID|name>` run one injection or unload at
  startup.
- Injected processes are tracked, hidden from the generic host's status window, and unloaded on exit
  through `VUnloadAll()` before `FreeLibrary`.
- `[CustomInject] Mode=minimal` in `DzjsTrainer.ini` skips JiYu-specific display/driver/remote-run
  hooks for custom hosts; default `full` installs the same 34-item hook table as StudentMain.

### Fixed

- Process icons that carry their own near-white plate no longer show as a white square on the
  selection row; every row draws its icon on a shared light rounded tile.

## [1.0.4.2] - 2026-11-14

### Fixed

- The update check no longer offers the same version forever on a build whose fourth version
  component is non-zero. `ReadOwnFileVersion` formatted the version as three components, so a
  `1.0.4.1` executable reported itself as `1.0.4`. It now returns all four, and the UI trims a
  trailing `.0` for display.

## [1.0.4.1] - 2026-11-14

### Fixed

- In-process update checks no longer fail with `SEC_E_SECPKG_NOT_FOUND`. Schannel and Negotiate
  credentials are acquired in `wWinMain`, before the security package cache is first needed.
- The `请求发送失败 err=183` line no longer reports a stale Win32 error; it reuses the status captured
  at the failing call.

### Changed

- The early-warmup result is carried in process globals and logged together with the pre-probe
  retest, because the log file is not open yet when `wWinMain` runs.

## [1.0.4] - 2026-11-14

### Security

- The update channel no longer trusts the manifest on its own word. The server signs a short-lived
  ticket over the manifest body, and the client verifies the signature, the nonce echo, the issue
  window and the SHA-256 of the body. A ticket older than the last accepted one is rejected.
- Only the public key ships in the client. The previous scheme used a shared HMAC secret, which put
  signing authority into every copy of the program.
- Both paths that write a downloaded package to disk refuse to run without a SHA-256 to check
  against, and refuse to apply it if the ticket check failed.
- The plain-text `checkupdate`, `getupdateinfo`, `getnewver` and `getupdate` endpoints are gone.

### Changed

- The public key is injected at build time from `UpdaterTicketKey.h`, generated by
  `tools/make-ticket-key.py`. A clone carries an example key and refuses tickets signed by the real
  server.
- The executables are packed with UPX.

### Fixed

- The updater reports why a manifest fetch failed instead of the generic "could not get update
  information".

## [1.0.2] - 2026-09-06

### Added

- Added a dedicated homepage/header action for super topmost.
- Added runtime UIAccess relaunch and token verification.

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
