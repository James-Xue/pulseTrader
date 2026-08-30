@echo off
rem configure_test.cmd — configure the test-only build tree (build_test/)
rem
rem Generates the unit-test solution and renames it to pulseTrader_test.sln
rem (CMake names every solution after project(pulseTrader); there is no
rem built-in way to override the generated .sln filename).
rem
rem Usage:
rem   configure_test.cmd [extra cmake args...]
rem
rem After this, open build_test\pulseTrader_test.sln in Visual Studio.
rem Note: re-running cmake (e.g. from the VS "Reconfigure" action) recreates
rem pulseTrader.sln — run this script again to rename it.

setlocal

set "CMAKE_OPTS="
:parse
if "%~1"=="" goto run
set "CMAKE_OPTS=%CMAKE_OPTS% %1"
shift
goto parse

:run
cmake -B build_test -S . -DPULSE_BUILD_TESTS=ON %CMAKE_OPTS%
if errorlevel 1 (
    echo [configure_test] cmake configuration failed.
    exit /b 1
)

if exist build_test\pulseTrader.sln (
    ren build_test\pulseTrader.sln pulseTrader_test.sln
    if errorlevel 1 (
        echo [configure_test] failed to rename build_test\pulseTrader.sln
        exit /b 1
    )
)

echo [configure_test] test solution ready: build_test\pulseTrader_test.sln
exit /b 0
