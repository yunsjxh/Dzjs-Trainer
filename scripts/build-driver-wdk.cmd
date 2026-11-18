@echo off
setlocal EnableExtensions
rem Build DzjsTrainerDriver.sys directly with cl/link from the WDK, bypassing
rem the MSBuild WDK toolset (which only registers into a full VS instance).
rem Output: <repo>\Release\DzjsTrainerDriver\DzjsTrainerDriver.sys (unsigned).

set "ROOT=%~dp0..\"
set "KITS=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "VCVARS=D:\VS\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARS%" (
  echo ERROR: vcvarsall.bat not found: %VCVARS%
  exit /b 2
)
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 3

set "DRV=%~dp0..\DzjsTrainerDriver"
set "OUT=%ROOT%Release\DzjsTrainerDriver"
set "OBJ=%ROOT%Release\obj-driver"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

set "INCS=/I"%DRV%" /I"%KITS%\Include\%SDKVER%\km" /I"%KITS%\Include\%SDKVER%\shared" /I"%KITS%\Include\%SDKVER%\ucrt""
set "DEFS=/D_WIN64 /D_AMD64_ /DAMD64 /D_NT_TARGET_VERSION=0xA000010 /DSUBSYSTEM_VERSION=10.00 /D_WIN32_WINNT=0x0A00 /D_WIN32_LEAN_AND_MEAN /DPOOL_NX_OPTIN=1"
set "CFLAGS=/nologo /c /kernel /GS- /Gs9999999 /W4 /WX- /O2 /Zi /Zc:wchar_t- /utf-8"

pushd "%OBJ%"
for %%F in (Driver64 DriverGuard64 Protection64 RegistryProtection64 UnloadToken64) do (
  echo [cl] %%F.c
  cl %CFLAGS% %DEFS% %INCS% /Fo"%%F.obj" "%DRV%\%%F.c"
  if errorlevel 1 (
    echo ERROR: compile failed for %%F.c
    popd
    exit /b 4
  )
)

echo [link] DzjsTrainerDriver.sys
link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
  /OUT:"%OUT%\DzjsTrainerDriver.sys" /PDB:"%OUT%\DzjsTrainerDriver.pdb" ^
  /LIBPATH:"%KITS%\Lib\%SDKVER%\km\x64" ^
  /LIBPATH:"%KITS%\Lib\%SDKVER%\ucrt\x64" ^
  Driver64.obj DriverGuard64.obj Protection64.obj RegistryProtection64.obj UnloadToken64.obj ^
  ntoskrnl.lib hal.lib wdm.lib wdmsec.lib cng.lib BufferOverflowK.lib
set "LINKRESULT=%ERRORLEVEL%"
popd
if not "%LINKRESULT%"=="0" (
  echo ERROR: link failed with %LINKRESULT%
  exit /b 5
)
echo BUILT: "%OUT%\DzjsTrainerDriver.sys"
exit /b 0
