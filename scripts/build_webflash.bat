@echo off
REM Project Voyager - Web Flash Build Script (Windows)
REM Builds RELEASE firmware (DEBUG=false) and LittleFS for every hardware version
REM and copies the binaries into webflash\.
REM
REM Usage: scripts\build_webflash.bat [--yes]
REM   --yes   non-interactive (no clean prompt, no pause)
REM
REM Manifest offsets are NOT regenerated here; scripts\build_webflash.py does that
REM from the built partitions.bin. Run the Python script for releases.
REM
REM Author: Aviral Verma
REM Repository: https://github.com/aviralverma-8877/Project-Voyager

setlocal enabledelayedexpansion

REM Run from the repo root regardless of the caller's cwd
pushd "%~dp0.."

set YES=0
if /i "%~1"=="--yes" set YES=1
if /i "%~1"=="-y" set YES=1

echo.
echo ============================================================
echo    Project Voyager - WebFlash Build Tool
echo ============================================================
echo.

REM Locate PlatformIO: %PIO%, then PATH, then the default install
if not defined PIO (
    where pio >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set PIO=pio
    ) else if exist "%USERPROFILE%\.platformio\penv\Scripts\pio.exe" (
        set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
    ) else (
        echo [ERROR] PlatformIO not found! Install it or set PIO=path\to\pio.exe
        echo Install from: https://platformio.org/install
        goto :fail
    )
)
echo [OK] PlatformIO: %PIO%

if %YES% EQU 0 (
    echo.
    set /p CLEAN="Clean previous builds? (y/N): "
    if /i "!CLEAN!"=="y" (
        echo [STEP] Cleaning previous builds...
        if exist .pio\build rmdir /s /q .pio\build
        echo [OK] Build directory cleaned
    )
)

for %%v in (1 2) do (
    echo.
    echo ============================================================
    echo    Building Hardware Version %%v ^(esp_lora_v%%v_release^)
    echo ============================================================
    "%PIO%" run -e esp_lora_v%%v_release
    if !ERRORLEVEL! NEQ 0 (
        echo [ERROR] Firmware build failed for version %%v!
        goto :fail
    )
    "%PIO%" run -e esp_lora_v%%v_release --target buildfs
    if !ERRORLEVEL! NEQ 0 (
        echo [ERROR] Filesystem build failed for version %%v!
        goto :fail
    )
    echo [OK] Version %%v firmware and filesystem built
)

if not exist webflash mkdir webflash

echo.
echo ============================================================
echo    Copying Files to WebFlash Directory
echo ============================================================

set "B1=.pio\build\esp_lora_v1_release"
set "B2=.pio\build\esp_lora_v2_release"
set "BOOT_APP0=%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
if defined PLATFORMIO_CORE_DIR set "BOOT_APP0=%PLATFORMIO_CORE_DIR%\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

call :copyfile "%B1%\firmware.bin"   "webflash\firmware-1.bin" || goto :fail
call :copyfile "%B1%\littlefs.bin"   "webflash\littlefs-1.bin" || goto :fail
call :copyfile "%B2%\firmware.bin"   "webflash\firmware-2.bin" || goto :fail
call :copyfile "%B2%\littlefs.bin"   "webflash\littlefs-2.bin" || goto :fail
call :copyfile "%B1%\bootloader.bin" "webflash\bootloader.bin" || goto :fail
call :copyfile "%B1%\partitions.bin" "webflash\partitions.bin" || goto :fail
call :copyfile "%BOOT_APP0%"         "webflash\boot_app0.bin"  || goto :fail

echo.
echo ============================================================
echo    Verifying WebFlash Files
echo ============================================================
set MISSING=0
for %%f in (bootloader.bin partitions.bin boot_app0.bin firmware-1.bin firmware-2.bin littlefs-1.bin littlefs-2.bin manifest-1.json manifest-2.json) do (
    if exist webflash\%%f (
        echo [OK] %%f
    ) else (
        echo [MISSING] %%f
        set MISSING=1
    )
)
if %MISSING% EQU 1 (
    echo [ERROR] Some files are missing from webflash directory!
    goto :fail
)

echo.
echo [SUCCESS] Binaries copied to %CD%\webflash
echo NOTE: manifests were not regenerated; run scripts\build_webflash.py for that.
popd
if %YES% EQU 0 pause
exit /b 0

:copyfile
if not exist "%~1" (
    echo [ERROR] Missing: %~1
    exit /b 1
)
copy /y "%~1" "%~2" >nul || exit /b 1
echo [OK] %~1 -^> %~2
exit /b 0

:fail
popd
if %YES% EQU 0 pause
exit /b 1
