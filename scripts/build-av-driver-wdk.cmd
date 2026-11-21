@echo off
setlocal EnableExtensions
rem Build JiYuAvKernel.sys directly with cl/link from the WDK.
rem Output: <repo>\JiYuAvRelease\JiYuAvKernel.sys (unsigned).

set "ROOT=%~dp0..\"
set "KITS=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
set "VCVARS="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSW%" for /f "usebackq tokens=*" %%I in (`"%VSW%" -latest -products * -find VC\Auxiliary\Build\vcvarsall.bat`) do if not defined VCVARS set "VCVARS=%%I"

if not exist "%VCVARS%" (
  echo ERROR: vcvarsall.bat not found: %VCVARS%
  exit /b 2
)
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 3

set "SRC=%ROOT%JiYuAvKernel"
set "SHARED=%ROOT%JiYuAvShared"
set "OUT=%ROOT%JiYuAvRelease"
set "OBJ=%OUT%\obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

rem Pick the newest SDK that actually ships kernel-mode headers.
if not defined SDKVER for /f "delims=" %%D in ('dir /b /ad /o-n "%KITS%\Include" 2^>nul') do (
  if not defined SDKVER if exist "%KITS%\Include\%%D\km" set "SDKVER=%%D"
)
if not defined SDKVER (
  echo ERROR: no Windows SDK with km headers found under "%KITS%\Include"
  exit /b 6
)
echo [env] vcvars=%VCVARS%
echo [env] sdk=%SDKVER%

set "INCS=/I"%SRC%" /I"%SHARED%" /I"%KITS%\Include\%SDKVER%\km" /I"%KITS%\Include\%SDKVER%\shared" /I"%KITS%\Include\%SDKVER%\ucrt""
set "DEFS=/D_WIN64 /D_AMD64_ /DAMD64 /D_NT_TARGET_VERSION=0xA000010 /DSUBSYSTEM_VERSION=10.00 /D_WIN32_WINNT=0x0A00 /D_WIN32_LEAN_AND_MEAN /DPOOL_NX_OPTIN=1"
set "CFLAGS=/nologo /c /kernel /GS- /Gs9999999 /W3 /WX- /O2 /Zi /utf-8"

pushd "%OBJ%"
for %%F in (Driver Controller Scanner SelfProtect SignatureStore) do (
  echo [cl] %%F.c
  cl %CFLAGS% %DEFS% %INCS% /Fo"%%F.obj" "%SRC%\%%F.c"
  if errorlevel 1 (
    echo ERROR: compile failed for %%F.c
    popd
    exit /b 4
  )
)

echo [link] JiYuAvKernel.sys
link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
  /OUT:"%OUT%\JiYuAvKernel.sys" /PDB:"%OUT%\JiYuAvKernel.pdb" ^
  /LIBPATH:"%KITS%\Lib\%SDKVER%\km\x64" ^
  /LIBPATH:"%KITS%\Lib\%SDKVER%\ucrt\x64" ^
  Driver.obj Controller.obj Scanner.obj SelfProtect.obj SignatureStore.obj ^
  ntoskrnl.lib hal.lib wdm.lib wdmsec.lib BufferOverflowK.lib cng.lib
set "LINKRESULT=%ERRORLEVEL%"
popd
if not "%LINKRESULT%"=="0" (
  echo ERROR: link failed with %LINKRESULT%
  exit /b 5
)
echo BUILT: "%OUT%\JiYuAvKernel.sys"
exit /b 0
