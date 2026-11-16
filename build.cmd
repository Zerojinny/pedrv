@echo off
rem Builds pedrv.exe with MSVC (static CRT, no runtime dependencies - runs in WinPE).
rem Usage: build.cmd [x64|x86|arm64]     (default x64)
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (echo vswhere.exe not found - install Visual Studio with the C++ workload & exit /b 1)
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio not found & exit /b 1)

set VCARCH=%ARCH%
if /i "%ARCH%"=="x86" set VCARCH=x64_x86
if /i "%ARCH%"=="arm64" set VCARCH=x64_arm64
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul || exit /b 1

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
