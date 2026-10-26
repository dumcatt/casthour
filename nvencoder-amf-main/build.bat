@echo off
rem Usage:  build.bat        (AMF backend, release)
rem         build.bat stub   (stub backend, no real encode)
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "BACKEND=AMF"
if /i "%~1"=="stub" set "BACKEND=STUB"
set "AMF=%ROOT%src"
set "AMF_SRC="

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" ( echo [build] could not find vcvars64.bat - install Visual Studio C++ tools & exit /b 1 )
call "!VCVARS!" >nul

if /i "%BACKEND%"=="STUB" goto :build

set "AMF=%ROOT%third_party\AMF\amf"
if not exist "%AMF%\public\common\AMFFactory.cpp" (
    echo [build] fetching AMD AMF SDK into third_party\AMF ...
    git clone --depth 1 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "%ROOT%third_party\AMF" || ( echo [build] git clone failed & exit /b 1 )
    git -C "%ROOT%third_party\AMF" sparse-checkout set amf/public/include amf/public/common || exit /b 1
)
set "AMF_SRC="%ROOT%src\amf_backend.cpp" "%AMF%\public\common\AMFFactory.cpp" "%AMF%\public\common\Thread.cpp" "%AMF%\public\common\TraceAdapter.cpp" "%AMF%\public\common\AMFSTL.cpp" "%AMF%\public\common\CurrentTimeImpl.cpp" "%AMF%\public\common\Windows\ThreadWindows.cpp""

:build
if not exist "%ROOT%dist"  mkdir "%ROOT%dist"
if not exist "%ROOT%build" mkdir "%ROOT%build"
echo [build] backend=%BACKEND%
cl /nologo /LD /O2 /EHsc /std:c++17 /DBACKEND_%BACKEND% ^
   /I "%ROOT%src" /I "%AMF%" ^
   "%ROOT%src\shim.cpp" %AMF_SRC% ^
   /Fe:"%ROOT%dist\nvEncodeAPI64.dll" /Fo:"%ROOT%build\\" ^
   /link /DEF:"%ROOT%src\nvenc_shim.def" d3d9.lib ole32.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> dist\nvEncodeAPI64.dll
