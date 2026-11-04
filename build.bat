@echo off
rem build.bat - configure and build RF-tools into .\build
rem
rem Usage: build.bat [Release|Debug|RelWithDebInfo|MinSizeRel] [extra cmake arguments...]
rem   The build type defaults to Release. Everything else is passed to the
rem   cmake configure step.
rem
rem Dependencies come from a project-local vcpkg, as in decode-orc:
rem   - vcpkg is cloned into .\vcpkg and bootstrapped on first use (a real git
rem     clone, since vcpkg resolves vcpkg.json's builtin-baseline from history)
rem   - vcpkg.json lists the libraries, and Qt 6 under the "qt" feature;
rem     vcpkg installs them into build\vcpkg_installed during configure.
rem     The first run builds Qt from source and takes a long time.
rem
rem Compiler: MSVC when cl.exe is on PATH (run from an "x64 Native Tools
rem Command Prompt for VS"), otherwise MinGW-w64 gcc from PATH.
rem
rem Environment overrides:
rem   VCPKG_TARGET_TRIPLET  vcpkg triplet (default: x64-windows with MSVC,
rem                         x64-mingw-dynamic with MinGW)
rem   USE_SYSTEM_QT=1       do not build Qt with vcpkg; point CMake at an
rem                         installed Qt with -DCMAKE_PREFIX_PATH=...
rem   NATIVE=0              portable build: no LTO / native CPU tuning
rem                         (on by default here, never in CI)
rem
rem Requires CMake and Git; Ninja is used when available.
rem
rem SPDX-License-Identifier: GPL-3.0-or-later
rem SPDX-FileCopyrightText: 2026 RF-tools contributors

setlocal EnableDelayedExpansion

rem Started by double-click (Explorer runs "cmd /c build.bat"): keep the window
rem open at the end so the result, or the error, can be read.
set "PAUSE_AT_END="
echo !cmdcmdline! | findstr /i /c:"/c" >nul && set "PAUSE_AT_END=1"

cd /d "%~dp0"

rem The first argument is the build type only when it names one; everything
rem else is passed to cmake configure verbatim. The raw argument string is
rem reused rather than re-tokenised, because batch splits on "=" and ";" and would
rem break values such as -DCMAKE_PREFIX_PATH=a;b.
set "BUILD_TYPE=Release"
set "EXTRA_ARGS=%*"
for %%T in (Release Debug RelWithDebInfo MinSizeRel) do (
    if /i "%~1"=="%%T" (
        set "BUILD_TYPE=%%T"
        set "EXTRA_ARGS=!EXTRA_ARGS:*%~1=!"
    )
)

rem --- project-local vcpkg -----------------------------------------------------
set "VCPKG_DIR=%CD%\vcpkg"

if not exist "%VCPKG_DIR%\.git" (
    echo Cloning vcpkg into vcpkg\ ...
    if exist "%VCPKG_DIR%" rmdir /s /q "%VCPKG_DIR%"
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_DIR%" || goto :error
)
if not exist "%VCPKG_DIR%\vcpkg.exe" (
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics || goto :error
)

rem --- compiler and triplet ----------------------------------------------------
set "USE_MSVC="
where cl >nul 2>&1 && set "USE_MSVC=1"

set "TRIPLET=%VCPKG_TARGET_TRIPLET%"
if "%TRIPLET%"=="" (
    if defined USE_MSVC (
        set "TRIPLET=x64-windows"
    ) else (
        set "TRIPLET=x64-mingw-dynamic"
    )
)

rem MinGW's windres runs the preprocessor without quoting its path, so a
rem toolchain under a directory with spaces ("C:\Program Files\mingw64") fails
rem on every .rc file (Qt's included). Put the 8.3 short form of gcc's directory
rem first on PATH so CMake and vcpkg pick up a space-free path.
if not defined USE_MSVC (
    set "GCC_DIR="
    for /f "delims=" %%G in ('where gcc 2^>nul') do if not defined GCC_DIR set "GCC_DIR=%%~dpG"
    if defined GCC_DIR (
        set "GCC_DIR=!GCC_DIR:~0,-1!"
        if not "!GCC_DIR: =!"=="!GCC_DIR!" (
            for %%D in ("!GCC_DIR!") do set "GCC_SHORT=%%~sD"
            if "!GCC_SHORT: =!"=="!GCC_SHORT!" (
                echo MinGW is under a path with spaces; using !GCC_SHORT!
                set "PATH=!GCC_SHORT!;!PATH!"
            ) else (
                echo WARNING: MinGW is under "!GCC_DIR!", a path with spaces, and no short
                echo name is available. windres will fail; install MinGW without spaces.
            )
        )
    )
)

rem MinGW has no MSVC to build vcpkg's host tools with, so the host triplet
rem follows the target one.
set "HOST_TRIPLET_ARG="
if not defined USE_MSVC set "HOST_TRIPLET_ARG=-DVCPKG_HOST_TRIPLET=%TRIPLET%"

set "FEATURES_ARG=-DVCPKG_MANIFEST_FEATURES=qt"
if "%USE_SYSTEM_QT%"=="1" set "FEATURES_ARG="

rem --- generator ---------------------------------------------------------------
rem Ninja when available (single-config, honours CMAKE_BUILD_TYPE); otherwise
rem MinGW Makefiles for gcc, or CMake's default Visual Studio generator.
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"
if not defined GENERATOR if not defined USE_MSVC set "GENERATOR=-G "MinGW Makefiles""


rem Local builds are tuned for this machine (LTO + native CPU); CI and the
rem release packages are not. Set NATIVE=0 for a portable build. Debug builds
rem never use it.
set "NATIVE_OPT=ON"
if "%NATIVE%"=="0" set "NATIVE_OPT=OFF"
if /i "%BUILD_TYPE%"=="Debug" set "NATIVE_OPT=OFF"

rem Version reported by every tool's --version: the git branch and short
rem commit, with -dirty when the working tree has uncommitted changes.
set "APP_BRANCH="
set "APP_COMMIT="
for /f "delims=" %%C in ('git rev-parse --short HEAD 2^>nul') do set "APP_COMMIT=%%C"
for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "APP_BRANCH=%%B"
set "VERSION_ARGS="
if defined APP_COMMIT (
    git diff --quiet HEAD >nul 2>&1 || set "APP_COMMIT=!APP_COMMIT!-dirty"
    if not defined APP_BRANCH set "APP_BRANCH=local"
    set "VERSION_ARGS=-DAPP_BRANCH=!APP_BRANCH! -DAPP_COMMIT=!APP_COMMIT!"
)

echo.
echo Build type: %BUILD_TYPE%   vcpkg triplet: %TRIPLET%   native optimisation: %NATIVE_OPT%
echo Version: %APP_BRANCH% / %APP_COMMIT%
echo.

cmake -S . -B build %GENERATOR% ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
    %HOST_TRIPLET_ARG% ^
    %FEATURES_ARG% ^
    -DENABLE_NATIVE_OPTIMIZATION=%NATIVE_OPT% ^
    %VERSION_ARGS% ^
    %EXTRA_ARGS% || goto :error

cmake --build build --config %BUILD_TYPE% --parallel || goto :error

rem --- run from the build tree -------------------------------------------------
rem Point the program at vcpkg's Qt plugins (platforms, iconengines)
rem so the program starts straight from build\bin.
set "QT_PLUGINS=%CD%\build\vcpkg_installed\%TRIPLET%\Qt6\plugins"
if /i "%BUILD_TYPE%"=="Debug" set "QT_PLUGINS=%CD%\build\vcpkg_installed\%TRIPLET%\debug\Qt6\plugins"
if exist "%QT_PLUGINS%" (
    > "build\bin\qt.conf" (
        echo [Paths]
        echo Plugins=!QT_PLUGINS:\=/!
    )
)

rem vcpkg copies only the DLLs an executable imports. Qt plugins have their
rem own: the SVG icon engine needs Qt6Svg.dll, which nothing imports, and
rem Windows would otherwise look for it on PATH and may find another Qt or
rem none (blank icons). Copy the whole vcpkg runtime next
rem to the binaries, as the Windows package does.
set "VCPKG_BIN=%CD%\build\vcpkg_installed\%TRIPLET%\bin"
if /i "%BUILD_TYPE%"=="Debug" set "VCPKG_BIN=%CD%\build\vcpkg_installed\%TRIPLET%\debug\bin"
if exist "%VCPKG_BIN%" copy /y "%VCPKG_BIN%\*.dll" "build\bin\" >nul

rem Without vcpkg's runtime (USE_SYSTEM_QT=1) the DLLs come from the Qt that
rem CMake was pointed at (CMAKE_PREFIX_PATH): through its windeployqt, which also
rem adds the plugins the window needs (platforms), or, where that Qt has none (a
rem vcpkg install), by copying its DLLs and pointing a qt.conf at its plugins.
if not exist "%VCPKG_BIN%" (
    set "QT_PREFIX="
    for /f "tokens=2 delims==" %%P in ('findstr /b /c:"CMAKE_PREFIX_PATH:" build\CMakeCache.txt 2^>nul') do (
        set "PREFIX_LIST=%%P"
        for %%Q in ("!PREFIX_LIST:;=" "!") do if not defined QT_PREFIX if exist "%%~Q\bin\Qt6Core.dll" set "QT_PREFIX=%%~Q"
    )
    if defined QT_PREFIX (
        if exist "!QT_PREFIX!\bin\windeployqt.exe" (
            for %%E in (rfx rf-requantize rf-analyse rf-packer) do (
                if exist "build\bin\%%E.exe" "!QT_PREFIX!\bin\windeployqt.exe" --no-translations "build\bin\%%E.exe" >nul
            )
        ) else (
            copy /y "!QT_PREFIX!\bin\*.dll" "build\bin\" >nul
            for %%L in ("!QT_PREFIX!\Qt6\plugins" "!QT_PREFIX!\plugins") do if exist "%%~L\platforms" (
                set "PLUGIN_DIR=%%~L"
                > "build\bin\qt.conf" (
                    echo [Paths]
                    echo Plugins=!PLUGIN_DIR:\=/!
                )
            )
        )
    ) else (
        echo Note: no Qt prefix found in build\CMakeCache.txt; the Qt DLLs are not copied.
    )
)

rem MinGW's own runtime, for the same reason: build\bin then runs without
rem depending on which MinGW (if any) is on PATH.
if defined GCC_DIR (
    for %%R in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do (
        if exist "!GCC_DIR!\%%R" copy /y "!GCC_DIR!\%%R" "build\bin\" >nul
    )
)

echo.
echo Build complete: binaries are in build\bin
if defined PAUSE_AT_END pause
exit /b 0

:error
echo.
echo Build failed. If it failed while vcpkg was building a dependency, the
echo details are in build\vcpkg-manifest-install.log and vcpkg\buildtrees\^<port^>\
if defined PAUSE_AT_END pause
exit /b 1
