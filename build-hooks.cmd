@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if exist "%VSWHERE%" for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%I"

if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not defined MSBUILD (
  echo ERROR: MSBuild was not found. Install Visual Studio 2022 C++ build tools.
  exit /b 2
)

 echo Using: %MSBUILD%
"%MSBUILD%" "%ROOT%DzjsTrainer.sln" /m /t:JiYuTrainerHooks /p:Configuration=Release /p:Platform=x86 /v:minimal /fl /flp:"logfile=%ROOT%build-hooks.log;verbosity=normal"
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo Build failed with exit code %RESULT%. See "%ROOT%build-hooks.log".
  exit /b %RESULT%
)

echo Build succeeded.
echo DLL: "%ROOT%Release\JiYuTrainerHooks.dll"
exit /b 0
