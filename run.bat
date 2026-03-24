@echo off
setlocal enableextensions enabledelayedexpansion

rem === Clone vcpkg if not present ===
if not exist "%~dp0vcpkg" (
    echo Cloning vcpkg...
    git clone https://github.com/microsoft/vcpkg.git "%~dp0vcpkg"
)

rem === Bootstrap vcpkg if not already done ===
if not exist "%~dp0vcpkg\vcpkg.exe" (
    echo Bootstrapping vcpkg...
    call "%~dp0vcpkg\bootstrap-vcpkg.bat" -disableMetrics
)

rem === Create build directory ===
if not exist "%~dp0build" mkdir "%~dp0build"

pushd "%~dp0build" || (
    echo Failed to enter build directory.
    exit /b 1
)

rem ---- Configure (Visual Studio 2022, x64) ----
echo Configuring with CMake...
cmake -G "Visual Studio 17 2022" -A x64 ..
if errorlevel 1 (
    echo CMake configure failed.
    popd
    pause
    exit /b 1
)

rem ---- Build (Release by default) ----
echo Building...
cmake --build . --config Release
if errorlevel 1 (
    echo Build failed.
    popd
    pause
    exit /b 1
)

popd

echo.
echo ========================================
echo  Build completed successfully!
echo ========================================
pause
