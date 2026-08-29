# PhoneDialogProbe

`PhoneDialogProbe.dll` is an x86 process-local diagnostic DLL for the `about.exe`
WinForms phone dialog. It records newly created or changed top-level windows and
their child controls to `%TEMP%\\PhoneDialogProbe-<pid>.log`.

It has no validation-changing behavior and does not authorize process termination.
The log identifies the WinForms window, the input control, and the sequence of UI
state changes so the managed validation method can be observed in an isolated test
environment. Keep the sample inside a disposable VM and deny it administrative
rights while collecting traces with Process Monitor/API Monitor.

Build for the target architecture:

```powershell
cmake -S . -B build-win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build-win32 --config Release
```

Inject the resulting DLL through the existing x86 `DllInjector` module, then open
the dialog and enter a test value. The resulting log is per-process and is written
inside the target process.
