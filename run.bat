@echo off
setlocal
cd /d "%~dp0"

call "build.bat"
if errorlevel 1 exit /b 1

set "OVMF_CODE=C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
set "OVMF_VARS_TEMPLATE=C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd"
set "OVMF_VARS_COPY=out\uefi\OVMF_VARS.fd"
set "ESP_PATH=%CD%\out\uefi\esp"

if not exist "%OVMF_CODE%" (
    echo Error: OVMF code firmware not found: %OVMF_CODE%
    exit /b 1
)
if not exist "%OVMF_VARS_TEMPLATE%" (
    echo Error: OVMF variable-store template not found: %OVMF_VARS_TEMPLATE%
    exit /b 1
)

where qemu-system-x86_64 >nul 2>nul
if errorlevel 1 (
    echo Error: qemu-system-x86_64 was not found. Install QEMU and add it to PATH.
    exit /b 1
)

copy /y "%OVMF_VARS_TEMPLATE%" "%OVMF_VARS_COPY%" >nul
if errorlevel 1 (
    echo Error: Could not copy the OVMF variable-store template.
    exit /b 1
)

qemu-system-x86_64 ^
    -machine q35 ^
    -m 512M ^
    -drive "if=pflash,format=raw,readonly=on,file=%OVMF_CODE%" ^
    -drive "if=pflash,format=raw,file=%OVMF_VARS_COPY%" ^
    -drive "format=raw,file=fat:rw:%ESP_PATH%" ^
    -serial stdio ^
    -no-reboot
exit /b %errorlevel%
