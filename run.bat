@echo off
cd /d "%~dp0"
if not exist "build\Release\black_hole.exe" (
    echo Not built yet - running setup.bat first...
    call setup.bat
    exit /b
)
rem Rebuild quickly in case you edited the code, then start
where cmake >nul 2>nul && cmake --build build --config Release --parallel >nul 2>nul
start "Black Hole" /d "%~dp0build\Release" "%~dp0build\Release\black_hole.exe"
