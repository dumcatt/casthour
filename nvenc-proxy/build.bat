@echo off
rem Usage:  build.bat          proxy DLL           -> dist\nvEncodeAPI64.dll
rem         build.bat test     proxy + test harness -> dist\harness.exe
setlocal enabledelayedexpansion
set "ROOT=%~dp0"

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" ( echo [build] could not find vcvars64.bat - install Visual Studio C++ build tools & exit /b 1 )
rem vcvars64.bat calls vswhere itself and complains if it isn't on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "!VCVARS!" >nul

if not exist "%ROOT%dist"  mkdir "%ROOT%dist"
if not exist "%ROOT%build" mkdir "%ROOT%build"

cl /nologo /LD /O2 /EHsc /std:c++17 /W3 /I "%ROOT%src" ^
   "%ROOT%src\proxy.cpp" "%ROOT%src\capture.cpp" "%ROOT%src\audio.cpp" "%ROOT%src\mfhook.cpp" ^
   "%ROOT%src\output.cpp" "%ROOT%src\tsmux.cpp" "%ROOT%src\common.cpp" ^
   /Fe:"%ROOT%dist\nvEncodeAPI64.dll" /Fo:"%ROOT%build\\" ^
   /link /DEF:"%ROOT%src\nvenc_proxy.def" d3d9.lib mfplat.lib mfuuid.lib ole32.lib user32.lib advapi32.lib ^
   wmcodecdspuuid.lib ws2_32.lib psapi.lib
if errorlevel 1 ( echo [build] proxy FAILED & exit /b 1 )
if not exist "%ROOT%dist\nvenc-proxy.ini" copy /y "%ROOT%nvenc-proxy.ini" "%ROOT%dist\" >nul
echo [build] OK -^> dist\nvEncodeAPI64.dll

if /i not "%~1"=="test" exit /b 0
cl /nologo /O2 /EHsc /std:c++17 /W3 /I "%ROOT%src" "%ROOT%test\harness.cpp" ^
   /Fe:"%ROOT%dist\harness.exe" /Fo:"%ROOT%build\\" ^
   /link d3d9.lib mf.lib mfplat.lib mfuuid.lib ole32.lib user32.lib advapi32.lib wmcodecdspuuid.lib
if errorlevel 1 ( echo [build] harness FAILED & exit /b 1 )
echo [build] OK -^> dist\harness.exe
