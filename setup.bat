@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title Black Hole - setup and build

echo ==========================================================
echo   Black Hole simulation - setup, build and run
echo ==========================================================
echo.
echo This script installs what is missing (CMake and the Microsoft
echo C++ compiler), downloads the libraries, builds and starts the
echo program. Windows may ask for permission (UAC) during installs.
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CMAKE_DEFAULT=%ProgramFiles%\CMake\bin\cmake.exe"

rem ---------------------------------------------------------------
rem  1. CMake
rem ---------------------------------------------------------------
set "CMAKE="
where cmake >nul 2>nul && set "CMAKE=cmake"
if not defined CMAKE if exist "%CMAKE_DEFAULT%" set "CMAKE=%CMAKE_DEFAULT%"
if not defined CMAKE (
    echo [1/4] Installing CMake with winget...
    call :need_winget || goto :fail
    winget install --id Kitware.CMake -e --source winget --accept-source-agreements --accept-package-agreements
    if exist "%CMAKE_DEFAULT%" set "CMAKE=%CMAKE_DEFAULT%"
)
if not defined CMAKE (
    echo CMake could not be installed automatically. Install it from https://cmake.org/download/ and run this again.
    goto :fail
)
echo [1/4] CMake OK

rem ---------------------------------------------------------------
rem  2. Microsoft C++ compiler (MSVC)
rem ---------------------------------------------------------------
call :find_msvc
if not defined VSPATH (
    echo [2/4] Installing Visual Studio 2022 Build Tools with the C++ workload.
    echo       This is a big download ^(a few GB^) and can take 10-30 minutes...
    call :need_winget || goto :fail
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --accept-source-agreements --accept-package-agreements --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    call :find_msvc
)
if not defined VSPATH (
    echo The C++ compiler was not found.
    echo Open "Visual Studio Installer", click Modify, tick "Desktop development with C++", install, then run this again.
    goto :fail
)
set "VSMAJOR=!VSVER:~0,2!"
set "GENERATOR="
if "!VSMAJOR!"=="17" set "GENERATOR=Visual Studio 17 2022"
if "!VSMAJOR!"=="18" set "GENERATOR=Visual Studio 18 2026"
echo [2/4] C++ compiler OK  ^(!VSPATH!^)

rem ---------------------------------------------------------------
rem  3. Configure + build (libraries are downloaded on first run)
rem ---------------------------------------------------------------
echo [3/4] Configuring and building - the first time takes a few minutes...
set "GENARGS="
if defined GENERATOR set GENARGS=-G "!GENERATOR!" -A x64 -DCMAKE_GENERATOR_INSTANCE="!VSPATH!"

"!CMAKE!" -B build -S . !GENARGS!
if errorlevel 1 (
    echo Configure failed - cleaning the build folder and trying once more...
    rmdir /s /q build 2>nul
    "!CMAKE!" -B build -S . !GENARGS! || goto :fail
)
"!CMAKE!" --build build --config Release --parallel || goto :fail

rem ---------------------------------------------------------------
rem  4. Run
rem ---------------------------------------------------------------
echo.
echo [4/4] Build finished!  Starting the simulation...
echo       Next time just double-click run.bat
echo.
start "Black Hole" /d "%~dp0build\Release" "%~dp0build\Release\black_hole.exe"
timeout /t 5 >nul
exit /b 0

rem ===============================================================
:find_msvc
set "VSPATH="
set "VSVER="
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VSVER=%%i"
exit /b 0

:need_winget
where winget >nul 2>nul && exit /b 0
echo winget was not found. Install "App Installer" from the Microsoft Store, then run this again.
exit /b 1

:fail
echo.
echo ******** Setup stopped. Read the messages above. ********
echo If you are stuck, open an issue on GitHub and include those messages.
pause
exit /b 1
