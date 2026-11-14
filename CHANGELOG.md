# Changelog

All notable changes to Dzjs Trainer are documented in this file.

## [1.0.5] - 2026-11-15

### Added

- The protection page now carries a driver-status chip next to "卸载主驱动": whether this
  process holds the driver device handle, whether the generated service exists and is running,
  and the service name. Three states rather than two, because `XDriverLoaded()` only reports the
  local handle — a driver loaded by another instance reads as "not loaded" there and sends people
  looking in the wrong place. The sample is cached for 1.5 seconds since `Paint` runs dozens of
  times a second during hover animations, and the cache is invalidated on load/unload.
- The exit dialog's verification code is now only valid for 30 seconds. When the window opens it starts a 200ms timer and stores an absolute deadline; every time the deadline passes it draws a new six-digit code, clears whatever has been typed, and drops the error state, because the old digits cannot match the new code and leaving them in place only makes the user think they mistyped. A `27 秒后刷新` caption sits at the right end of the code row so the code does not change without warning — 11px, right-aligned, with no background or border, so it adds no new geometry. Remaining seconds are computed from the deadline rather than decremented per tick: `WM_TIMER` is low priority and each tick can arrive a couple of hundred milliseconds late, which would make a decrementing counter visibly drift. Repaints are clipped to the countdown's own 92×38 rectangle instead of invalidating the whole window once a second.
- Four hooks ported from `R3ShieldCore` close real coverage gaps: `PrintWindow` (`hk42`), `GetDIBits` (`hk43`) and `StretchBlt` (`hk44`) in the display domain, and `SetWindowsHookExW` (`hk45`) in the input domain. Screen capture never went only through `BitBlt` — and that hook is disabled in the full set — so `GetDC(NULL)` followed by `GetDIBits`, or a scaled `StretchBlt`, or `PrintWindow` on a window handle all reached the pixels without passing a hooked function. Likewise `SetWindowsHookExA` was hooked but `SetWindowsHookExW` was not, and callers overwhelmingly use the W form. The three capture detours share the reference project's filter: the source DC must satisfy `WindowFromDC(dc) == desktop window`, so memory DCs pass straight through and the trainer's own drawing is untouched. No new switch is needed — they reuse `AllowMonitor` and `AllowControl`.
- Image replacement now runs on those generic GDI hooks instead of only inside JiYu's own encoder. `StretchBlt`, `PrintWindow` and `GetDIBits` consult the same three-state decision as the JPEG path (`ReplaceCodec::PlanJpegFrame`) and draw from the same `g_frames` store, so picking a different image in the UI changes every capture path at once rather than only `LibJPEG20!EncodeToJPEGBuffer`. `GetDIBits` paints the caller's raw buffer for 24- and 32-bpp targets and falls back to a solid fill for 8/16-bpp; the solid colour keeps the existing meaning — white when "允许监看" is off, black when replacement is on without an image. `BitBlt` keeps its existing backing-bitmap renderer, which already supported custom images.

### Changed

- Both keys behind driver unload authorization were rotated. The ECDSA P-256 pair that signs the unload token is new, which means the public key compiled into `UnlockPublicKey.h` changed and the driver had to be rebuilt; an old signed build will reject every token issued from now on, and a token from before the rotation is rejected by the new key. The AES-256 key that wraps the token before it goes into the executable is new as well. It is still stored as two 32-byte halves XORed together at runtime rather than as a single array, so a hex dump of the binary does not show the key, but the halves changed, and all three places that carry them — the protect script, the verify script and `KernelUtils.cpp` — were updated together so an encrypted token cannot silently stop decrypting. A freshly issued token verifies against the new public key, the old token still verifies against the old key, and the old token is rejected by the new key.
- Image replacement is no longer confined to JiYu's own processes. The hooks DLL used to force `EnableVideoModify` off the moment it landed in a process it did not recognise, because leaving it on sent `VInstallHooks` down the JiYu-only "minimal frame replacement" branch — that branch installs hooks for three JiYu modules and returns, so the ordered 34-hook table never ran. The branch now checks the host first, which means a third-party process gets the full table and, on top of it, the `BitBlt` hook the table deliberately skips. Inject any program and its screen captures come back as the chosen image. `BitBlt` needs its own filter there: a capture-helper process can be blackened wholesale because capturing is all it does, while a browser or a media player paints its own window through the same function, so only a `SRCCOPY` from a DC whose `WindowFromDC` is the desktop into a memory DC is replaced — every other direction passes through untouched. Signature comes from the same helper the three capture hooks use, so the two definitions cannot drift apart. `[CustomInject] Mode=minimal` still skips the whole display domain, and replacement does not work in that mode — that is now written to the log rather than failing silently.
- The blurred desktop behind the exit dialog was rendering as a flat dark green field instead of a recognisable desktop. Three layers were each tinted green: the base fill, the downscaled wallpaper's own clear colour, and the dimming pass — and the dimming pass was the strongest of them. The wallpaper is blue on a default installation, so the green cast pulled it to grey-green while the heavy dim removed whatever structure was left. All three are now neutral dark grey, so the wallpaper's own colours come through. The blur is also tightened — 320×180 at radius 3 instead of 240×135 at radius 4 — which is roughly 18px at 1080p rather than 32px, so large structures stay recognisable.
- The exit dialog's full-screen surface no longer carries `WS_EX_DLGMODALFRAME` or `WS_BORDER`, which drew a 1px system frame around the edge of the dimmed wallpaper.
- The exit dialog's code field lost `WS_EX_CLIENTEDGE` and now sits inside a self-drawn 1px rounded outline, because the native 3D sunken border was the only non-rounded control edge in the whole application. Its fill matches the panel, so it reads as an outlined field rather than a nested card. `WM_CTLCOLOREDIT` sets the background to the same colour the brush uses, which it did not before — mismatched values leave a patch behind once text is typed and deleted.
- The exit dialog's vertical rhythm is even. The code, the hint line, the error line and the field used to sit at uneven gaps (32px above the hint, 4px above the error line) because the error line reserved its slot in the middle of the block; apart from the error line, which stays tight against the hint, the inner gaps are now a uniform 16px. Only four offsets moved, by 8px at most — panel size, control sizes and type scale are untouched.
- The exit dialog's two buttons are registered for hover tracking, which they previously lacked, and the dialog now draws with antialiased geometry and text instead of the default non-antialiased mode.
- The exit dialog's panel geometry is a single block of constants shared by `PaintExitConfirm` and `WM_CREATE`. The two used to carry their own hardcoded coordinates, so any adjustment to one would silently misalign the native controls against the painted frame.
- The wallpaper blur behind the exit dialog is separable: a horizontal pass followed by a vertical one instead of a two-dimensional `(2r+1)²` kernel. The result is equivalent up to one colour step of rounding, and the sample count drops from 2.62M to 1.04M despite the higher resolution.
- The update flow now lives entirely in the standalone `DzjsTrainerUpdater.exe`. The main program no longer downloads, verifies, or applies anything; the update entry on the About page extracts the embedded updater next to the main program and starts it.
- The updater checks the published version itself, downloads the package, verifies its SHA-256, and then asks the user to close the main program before replacing it. It polls until the file is no longer locked instead of waiting on a parent process id.
- The updater now runs from the install directory instead of `%TEMP%`, and requests elevation itself so a declined prompt is reported instead of being treated as success.
- Certificate validation is enabled on the update channel.
- Every hook is record-only by default. A detour now checks its feature switch first; when the switch is off it writes one `RECORD` line to `DzjsTrainerHooks-debug.log` and calls the original function unchanged, so a fresh install observes what JiYu tries to do instead of blocking it. Turning on a switch in "防护策略" is what makes the matching hook block or replace. Two options are new — "拦截远程窗口操作" (`BlockWindowOps`) and "拦截极域驱动访问" (`BlockDriverAccess`) — because window operations and the TD driver handles previously had no switch at all and acted unconditionally. "阻止远程结束进程" now also covers `ExitWindowsEx`, and the hooks DLL stops injecting into `DispcapHelper.exe` unless video-stream protection is on. Existing installations are migrated once: the shipped defaults that the old version wrote (`ProhibitKillProcess=TRUE`, `ProhibitCloseWindow=TRUE`, `AllowControl=FALSE`) are reset to the record-only values and marked with `EnforceMode=record`, so later changes by the user are kept as-is.
- The window's light/dark theme follows the Windows app theme. The startup value is read from `AppsUseLightTheme` (falling back to `SystemUsesLightTheme` when the key is absent), and `WM_SETTINGCHANGE` / `WM_THEMECHANGED` re-read it while the window is open, so switching the system between light and dark repaints the window immediately. The non-client title bar is switched through `DWMWA_USE_IMMERSIVE_DARK_MODE` as well, otherwise it stayed light above a dark client area. Clicking the title-bar theme button switches to a manual choice for the rest of the session; later system changes no longer override it.

### Fixed

- The driver signature policy rejected the program's own embedded driver. The pin still carried
  the old test certificate (`JiYuTrainerDriver-Yunsjxh`) while the release driver was signed with
  `DzjsTrainerDriver By:Yunsjxh`, and the publisher allowlist only knew the old names, so every
  machine refused to load it. The pin now holds the current subject and thumbprint, and the
  decision checks the trust status before consulting it: a broken digest or an expired
  certificate is still rejected even when the pinned signer matches.
- `Build-DzjsTrainerRelease.ps1` derived the pin with `Subject -replace '^CN='`. A certificate
  whose subject carries `O=` and `C=` had the whole distinguished name written into the pin,
  which can never match the simple display name the program compares against. It now uses
  `GetNameInfo(SimpleName)`, the same call the runtime makes.
- `GuardTerminator` waited the full 60 seconds for a guard process that was not installed, then
  logged an error every cycle. It returns immediately when nothing is found, and a guard that
  ends without being respawned is a warning rather than an error.
- Unloading custom injection at exit opened processes that had already terminated, and reported
  `20002` / `0xC000000B` for each. Targets are checked first and skipped when they are gone.
- The process picker's search field could not be typed into under a Chinese input method, and had no caret to show it was focused at all. It was not a control: it was a rounded rectangle painted into the card, with the characters arriving as `WM_CHAR` on the main window. That works for Latin input and for nothing else. An IME consumes the letter keys into a composition string and delivers the result as `WM_IME_CHAR`, which the window never handled, so the field stayed empty no matter what was typed — and since the caret was only drawn once the filter was non-empty, an empty field gave no sign of being focused at all. It is now a real `EDIT` child window, positioned inside the painted outline with a 10px horizontal and 7px vertical inset so the rounded corners stay visible, filled through `WM_CTLCOLOREDIT` with the same colour as the card's own field, and carrying the placeholder as a cue banner instead of painted text. Focus, caret, selection, the system clipboard, the context menu and IME composition all come from the control; `EN_CHANGE` feeds the filter, and a subclass forwards the keys the list owns — arrows, page up/down, Enter, Esc — plus the wheel, which the edit would otherwise use to scroll its own text. `Tab` was being swallowed by the picker's "eat every key" default, so it now moves the focus ring around the picker's four buttons; that path existed already and was simply unreachable. Characters typed while the focus ring sits on a button still go through `WM_CHAR`, and that path now writes back into the edit so the two views of the filter cannot drift apart.
- `CreateDCW(L"DISPLAY")` was a hole in the capture filter. Every screen-capture detour decided "is this the desktop" with `WindowFromDC(dc) == desktop window`, which is true for `GetDC(NULL)` and `GetWindowDC(desktop)` but false for a device context created through `CreateDCW` — the API returns no window at all, so the check reported a memory DC and the whole path was waved through. A capture tool that opens its own display DC and blits from it got the real screen while every other probe path came back replaced. The filter now returns two kinds instead of a boolean: a window DC when `WindowFromDC` is the desktop, and a display DC when there is no window but `GetObjectType` says `OBJ_DC` and the device technology is `DT_RASDISPLAY` — printers are `OBJ_DC` too and are excluded by the technology test rather than by a size guess. A display DC is also a legitimate drawing target (older software blits straight onto the screen DC), so the display branch additionally requires the destination to be a memory DC; the window branch keeps its previous behaviour untouched, which is what JiYu's own processes exercise. `GetDIBits` gets a stricter rule still, because there the `hdc` only supplies a colour format while the pixels come from the bitmap — a display DC is accepted only when the bitmap is at least screen-sized, so reading an unrelated bitmap with a display DC as format context is left alone.
- 16-bpp `GetDIBits` targets were filled solid black instead of getting the replacement image. The buffer writer handled 24- and 32-bpp only and deliberately refused everything else, falling back to the solid fill — safe, since real pixels never escaped, but a 16-bpp grab reported a flat black frame rather than the chosen picture. It now packs 555 when `biCompression` is `BI_RGB` and 565 when it is `BI_BITFIELDS` with the standard `0xF800/0x07E0/0x001F` masks, and still falls back to the solid fill for any other mask layout rather than guessing. 8-bpp stays on the solid fill, since writing the buffer there means quantising through a palette the caller may have filled in.
- Owner-drawn buttons no longer show a pale halo around their rounded corners. A `BS_OWNERDRAW` button is a rectangular window: it erases itself with whatever brush the parent returns from `WM_CTLCOLORBTN`, and only then paints. `DrawNativeButton` filled a rounded rect and nothing else, so the four corners kept the erase colour — `COLOR_BTNFACE`, `RGB(240,240,240)`, which is what `DefWindowProc` hands back when the parent does not handle the message. That was invisible while the cards were pure white and became a white ring the moment the cards went dark. `DrawNativeButton` now takes the surface colour it sits on and fills the whole button rectangle with it before drawing, and both window procedures answer `WM_CTLCOLORBTN` so the erase pass matches too. Measured on a live `BS_OWNERDRAW` button over a `RGB(38,45,42)` panel: 315 light pixels inside the button rect before, 50 after, with the 50 being the button's own text; corner samples went from `RGB(240,240,240)` to the panel colour. The exit dialog's panel colour also moved into a single `ExitPanelColor()` helper instead of being repeated at three call sites.
- The exit dialog no longer flashes a black frame each time the countdown ticks. The paint routine wrote to the window DC in four separate steps — base fill, wallpaper blit, dimming pass, then the panel — and the compositor is free to take a frame between any two of them; once-a-second partial repaints made that intermediate frame, which is nothing but the base fill, far easier to catch than a full repaint would have been. Everything is now rendered into an `ExitBackBuffer` — a `CreateDIBSection` surface with a top-down `biHeight` so GDI+ coordinates match, wrapped by a `Gdiplus::Bitmap` over the same memory — and handed to the window in a single `BitBlt`. `ps.rcPaint` drives both the GDI+ clip and the blit rectangle, so the per-second repaint stays limited to the countdown instead of redrawing the whole screen. Measured on a live window with a 4ms capture loop: minimum luma over the countdown area was 18 before (exactly the base fill colour) and 46 after. The wrapper bitmap is released before the DIB, since it only points at that memory, and `WM_NCDESTROY` destroys the buffer so a resolution change does not leak a DC and a DIB.
- The download URL from the manifest is decoded as UTF-8 and its host is converted to punycode before use. Decoding it with the ANSI code page corrupted the host, so every download failed with a connection error.
- `CompareVersions` treats a missing component as zero, so `1.0.5` and `1.0.5.0` no longer compare as different.
- The updater's version comparison reads the installed program's file version rather than the updater's own build-time constant.

### Removed

- The in-app download window, its progress reporting, and the payload/parent-pid helper path.
- The `AutoUpdate` setting and the startup update timer. The new flow requires the user to close the main program, so an unattended check cannot work.
- The "使用帮助" page is gone from the sidebar; its slot is taken by "进程注入", which moves up from the last position. Injection is a working page and was buried behind four unrelated ones. The tray menu's "帮助" item no longer jumps to a page — it shows the same short help message the broadcast-window menu already used.

## [1.0.4.3] - 2026-11-14

### Added

- Custom process injection: the new "进程注入" page injects the hooks DLL into a user-specified PID or process name. Both 32-bit and 64-bit targets are supported; 64-bit targets go through a new embedded x64 injector helper and an x64 build of the hooks DLL.
- The injection page picks targets from a built-in process list instead of a native text prompt: every row shows the executable's icon, PID, image path, target bitness and whether it is already injected. Type to filter by name, path or PID; arrow keys move the cursor, Space toggles a row, Enter injects, Esc cancels. Icons are read lazily through `SHGetFileInfoW` and cached per image path, so opening the list does not wait on the shell for every process on the machine.
- The process list is multi-select: clicking a row toggles it, Shift+click selects a range, Ctrl+A or the "全选" button selects everything the current filter shows. The action button shows the count ("注入 3 个") and processes the whole selection, reporting how many succeeded and naming the ones that failed.
- System-critical processes are no longer exempt from injection. They are still tagged "系统" as a warning, but they can be selected and injected like any other target — the list of protected names was only ever a hardcoded guess, while the actual target set is chosen by hand. Only the trainer's own process stays non-selectable, because loading the hooks into it would hook the trainer's own file and registry calls. PID-based rejection of 0-4 was dropped for the same reason: the attempt now runs and the injector reports the real error.
- CLI switches `-inject <PID|name>` and `-uninject <PID|name>` run one injection/unload at startup.
- Injected processes are tracked, hidden from the generic host's status window, and unloaded on exit. Unloading calls the exported `VUnloadAll()` before `FreeLibrary`, so no dangling detours are left behind.
- `[CustomInject] Mode=minimal` in `DzjsTrainer.ini` skips JiYu-specific display/driver/remote-run hooks for custom hosts; default `full` installs the same 34-item ordered hook table as StudentMain.
### Fixed

- Process icons that carry their own near-white plate — the shell's generic window icon, returned for executables without an icon resource — no longer show up as a white square on the mint selection row. Every row now draws its icon on a shared light rounded tile, so plate-backed and transparent icons sit on the same surface, in both themes.

## [1.0.4.2] - 2026-11-14

### Fixed

- The update check no longer offers the same version forever on a build whose fourth
  version component is non-zero. `ReadOwnFileVersion` formatted the running binary's
  version as three components, so a `1.0.4.1` executable reported itself as `1.0.4`
  while the manifest said `1.0.4.1`; `CompareVersions` pads the missing component with
  zero, concluded the remote copy was newer, and every check re-downloaded the exact
  same package. The reader now returns all four components, and the UI trims a
  trailing `.0` only for display, so `1.0.4.0` still reads as `1.0.4`.

## [1.0.4.1] - 2026-11-14

### Fixed

- In-process update checks no longer fail every time with `SEC_E_SECPKG_NOT_FOUND`
  (`0x80090305`). The security package list is a per-process cache inside `secur32`,
  filled on first use; when that first enumeration failed the cache stayed empty for
  the rest of the process, so every later `AcquireCredentialsHandle` — including the
  one WinHTTP makes inside `WinHttpSendRequest` — returned the same code. The process
  now acquires Schannel and Negotiate credentials in `wWinMain`, before any driver is
  loaded or privileges are raised, which populates the cache correctly. Measured on
  the user's machine: `路径=进程内 ... 耗时=594 ms`, full TLS handshake and ticket
  verification, no more fallback to the updater subprocess.
- The `请求发送失败 err=183` line no longer reports a stale Win32 error.
  `WinHttpSendRequest` does not set the last error when it fails, so the old log line
  was printing whatever an earlier call had left behind; it now reuses the status
  captured at the failing call.

### Changed

- The early-warmup result is carried in process globals and logged together with the
  pre-probe retest, because the log file is not open yet when `wWinMain` runs.

## [1.0.4] - 2026-11-14

### Security

- The update channel no longer trusts the manifest on its own word. The server signs a
  short-lived ticket over the manifest body with an RSA-2048 key and returns it in the
  `X-Dzjs-Ticket` header; the client verifies the signature, the nonce echo, the issue
  window, and the SHA-256 of the body before using a single byte of it. A ticket older
  than the last accepted one is rejected, so rolling the clock back does not revive it.
- Only the public key ships in the client. The previous scheme used a shared HMAC secret,
  which put signing authority into every copy of the program; publishing the source would
  have published the ability to forge tickets.
- Both paths that can write a downloaded package to disk now refuse to run without a
  SHA-256 to check it against, and refuse to apply it if the ticket check failed.
- The plain-text `checkupdate`, `getupdateinfo`, `getnewver`, and `getupdate` endpoints are
  gone. They handed out the version number and the download URL without any credential,
  which made the signature check irrelevant.

### Changed

- The public key is injected at build time from `UpdaterTicketKey.h`, generated by
  `tools/make-ticket-key.py`. A clone of this repository carries an example key instead,
  compiles without any manual step, and refuses to accept tickets signed by the real
  server rather than failing with a vague signature error.
- The executables are packed with UPX.

### Fixed

- The updater reports why a manifest fetch failed instead of the generic "could not get
  update information".

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
