@echo off
REM Project Voyager - Web Flash Build Script (Windows)
REM Builds firmware and filesystem for all hardware versions
REM Author: Aviral Verma
REM Repository: https://github.com/aviralverma-8877/Project-Voyager

setlocal enabledelayedexpansion

echo.
echo ============================================================
echo    Project Voyager - WebFlash Build Tool
echo ============================================================
echo Author: Aviral Verma
echo Repository: https://github.com/aviralverma-8877/Project-Voyager
echo.

REM Check if PlatformIO is installed
where pio >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] PlatformIO not found! Please install PlatformIO first.
    echo Install from: https://platformio.org/install
    pause
    exit /b 1
)
echo [OK] PlatformIO found

REM Ask to clean previous builds
echo.
set /p CLEAN="Clean previous builds? (y/N): "
if /i "%CLEAN%"=="y" (
    echo [STEP] Cleaning previous builds...
    if exist .pio\build (
        rmdir /s /q .pio\build
        echo [OK] Build directory cleaned
    ) else (
        echo [OK] No previous builds to clean
    )
)

REM Build firmware for version 1
echo.
echo ============================================================
echo    Building Firmware for Hardware Version 1
echo ============================================================
echo.
echo [STEP] Building esp_lora_v1 firmware...
pio run -e esp_lora_v1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed for version 1!
    pause
    exit /b 1
)
echo [OK] Version 1 firmware built successfully

REM Build firmware for version 2
echo.
echo ============================================================
echo    Building Firmware for Hardware Version 2
echo ============================================================
echo.
echo [STEP] Building esp_lora_v2 firmware...
pio run -e esp_lora_v2
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed for version 2!
    pause
    exit /b 1
)
echo [OK] Version 2 firmware built successfully

REM Build filesystem
echo.
echo ============================================================
echo    Building LittleFS Filesystem
echo ============================================================
echo.
echo [STEP] Building LittleFS filesystem...
pio run -e esp_lora_v1 --target buildfs
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Filesystem build failed!
    pause
    exit /b 1
)
echo [OK] Filesystem built successfully

REM Create webflash directory if it doesn't exist
if not exist webflash mkdir webflash

REM Copy files to webflash directory
echo.
echo ============================================================
echo    Copying Files to WebFlash Directory
echo ============================================================
echo.

REM Copy version 1 files
echo [STEP] Copying files for version 1...
if exist .pio\build\esp_lora_v1\firmware.bin (
    copy /y .pio\build\esp_lora_v1\firmware.bin webflash\firmware-1.bin >nul
    echo [OK] Copied firmware-1.bin
) else (
    echo [ERROR] firmware.bin not found for version 1!
)

if exist .pio\build\esp_lora_v1\littlefs.bin (
    copy /y .pio\build\esp_lora_v1\littlefs.bin webflash\littlefs-1.bin >nul
    echo [OK] Copied littlefs-1.bin
) else (
    echo [WARNING] littlefs.bin not found for version 1!
)

REM Copy version 2 files
echo [STEP] Copying files for version 2...
if exist .pio\build\esp_lora_v2\firmware.bin (
    copy /y .pio\build\esp_lora_v2\firmware.bin webflash\firmware-2.bin >nul
    echo [OK] Copied firmware-2.bin
) else (
    echo [ERROR] firmware.bin not found for version 2!
)

if exist .pio\build\esp_lora_v2\littlefs.bin (
    copy /y .pio\build\esp_lora_v2\littlefs.bin webflash\littlefs-2.bin >nul
    echo [OK] Copied littlefs-2.bin
) else (
    echo [WARNING] littlefs.bin not found for version 2!
)

REM Copy common files (bootloader, partitions, boot_app0)
echo [STEP] Copying common files...
if exist .pio\build\esp_lora_v1\bootloader.bin (
    copy /y .pio\build\esp_lora_v1\bootloader.bin webflash\bootloader.bin >nul
    echo [OK] Copied bootloader.bin
) else (
    echo [WARNING] bootloader.bin not found!
)

if exist .pio\build\esp_lora_v1\partitions.bin (
    copy /y .pio\build\esp_lora_v1\partitions.bin webflash\partitions.bin >nul
    echo [OK] Copied partitions.bin
) else (
    echo [WARNING] partitions.bin not found!
)

REM Try to find boot_app0.bin
if exist .pio\build\esp_lora_v1\boot_app0.bin (
    copy /y .pio\build\esp_lora_v1\boot_app0.bin webflash\boot_app0.bin >nul
    echo [OK] Copied boot_app0.bin
) else if exist .platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin (
    copy /y .platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin webflash\boot_app0.bin >nul
    echo [OK] Copied boot_app0.bin
) else (
    echo [WARNING] boot_app0.bin not found!
)

REM Verify files
echo.
echo ============================================================
echo    Verifying WebFlash Files
echo ============================================================
echo.

set MISSING=0

for %%f in (bootloader.bin partitions.bin boot_app0.bin firmware-1.bin firmware-2.bin littlefs-1.bin littlefs-2.bin manifest-1.json manifest-2.json) do (
    if exist webflash\%%f (
        echo [OK] %%f
    ) else (
        echo [MISSING] %%f
        set MISSING=1
    )
)

echo.
if %MISSING% EQU 1 (
    echo [WARNING] Some files are missing from webflash directory!
) else (
    echo [SUCCESS] All files present in webflash directory!
)

REM Success message
echo.
echo ============================================================
echo    Build Complete!
echo ============================================================
echo.
echo All firmware and filesystem files have been built and copied to:
echo %CD%\webflash
echo.
echo You can now use these files with the ESP Web Tools flasher.
echo.

pause
