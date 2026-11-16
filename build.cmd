@echo off
rem Builds pedrv.exe with MSVC (static CRT, no runtime dependencies - runs in WinPE).
rem Usage: build.cmd [x64|x86|arm64] [toolset]     (default x64)
rem   toolset: MSVC version to use, e.g. 14.44. Default: the default toolset of Visual Studio
rem            if it can build for the architecture, else the newest installed one that can
rem            (e.g. 14.44 for arm64 when the default toolset has no ARM64 compiler/libraries).
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set VCVER=%2

rem vcvarsall.bat runs vswhere.exe from PATH; without this it prints "'vswhere.exe' is not recognized".
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (echo vswhere.exe not found - install Visual Studio with the C++ workload & exit /b 1)
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio not found & exit /b 1)

set VCARCH=%ARCH%
if /i "%ARCH%"=="x86" set VCARCH=x64_x86
if /i "%ARCH%"=="arm64" set VCARCH=x64_arm64

rem ---- MSVC toolset: one with a compiler for the architecture and the static C++ runtime ----
set "VCROOT=%VSDIR%\VC\Tools\MSVC"
if not defined VCVER goto :findtoolset
for /f "delims=" %%v in ('dir /b /ad /o-n "%VCROOT%\%VCVER%*" 2^>nul') do call :cantarget %%v && goto :toolset
echo Toolset %VCVER% cannot build %ARCH% (no %ARCH% compiler or static runtime). Installed:
dir /b /ad "%VCROOT%"
exit /b 1
:findtoolset
set DEFVER=
set /p DEFVER=<"%VSDIR%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
if defined DEFVER call :cantarget %DEFVER% && goto :toolset
for /f "delims=" %%v in ('dir /b /ad /o-n "%VCROOT%"') do call :cantarget %%v && (set VCVER=%%v& goto :toolset)
echo No MSVC toolset can build %ARCH% - add "MSVC %ARCH% build tools" in the Visual Studio Installer
exit /b 1
:toolset
set VCOPT=
if defined VCVER set VCOPT=-vcvars_ver=%VCVER%
if defined VCVER (echo Toolset: %VCVER%) else echo Toolset: %DEFVER% (default)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% %VCOPT% >nul || exit /b 1

set OUT=%~dp0bin\%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /MT /EHsc /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS "%~dp0src\pedrv.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\pedrv.exe" /link /OPT:REF /OPT:ICF || exit /b 1
del "%OUT%\*.obj" 2>nul

rem Bundle 7-Zip (needed for .7z/.zip sources) when it matches the build architecture.
if /i "%ARCH%"=="x64" if exist "%ProgramFiles%\7-Zip\7z.exe" (
    copy /y "%ProgramFiles%\7-Zip\7z.exe" "%OUT%\" >nul
    copy /y "%ProgramFiles%\7-Zip\7z.dll" "%OUT%\" >nul
)
echo Built %OUT%\pedrv.exe
exit /b 0

rem Succeeds if toolset %1 has an x64-hosted compiler for %ARCH% and its static C++ runtime.
:cantarget
if not exist "%VCROOT%\%1\bin\Hostx64\%ARCH%\cl.exe" exit /b 1
if not exist "%VCROOT%\%1\lib\%ARCH%\libcpmt.lib" exit /b 1
exit /b 0
