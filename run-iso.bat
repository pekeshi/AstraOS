@echo off
setlocal
cd /d "%~dp0"

call "build.bat"
if errorlevel 1 goto :build_failed

set "MSYS_BIN=C:\msys64\mingw64\bin"
set "XORRISO=C:\msys64\usr\bin\xorriso.exe"
set "OVMF_CODE=C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
set "OVMF_VARS_TEMPLATE=C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd"
set "OVMF_VARS_COPY=out\uefi\OVMF_VARS_ISO.fd"
set "ISO_ROOT=out\uefi\iso-root"
set "EFI_IMAGE=%ISO_ROOT%\efiboot.img"
set "ISO_PATH=out\AstraOS.iso"

if not exist "%MSYS_BIN%\qemu-img.exe" (
    echo Error: qemu-img was not found in %MSYS_BIN%.
    exit /b 1
)
if not exist "%MSYS_BIN%\mformat.exe" (
    echo Error: mformat was not found in %MSYS_BIN%. Install MSYS2 mtools.
    exit /b 1
)
if not exist "%MSYS_BIN%\mcopy.exe" (
    echo Error: mcopy was not found in %MSYS_BIN%. Install MSYS2 mtools.
    exit /b 1
)
if not exist "%XORRISO%" (
    echo Error: xorriso was not found: %XORRISO%
    exit /b 1
)
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

if exist "%ISO_ROOT%" rmdir /s /q "%ISO_ROOT%"
if errorlevel 1 (
    echo Error: Could not clear the ISO staging directory.
    exit /b 1
)
mkdir "%ISO_ROOT%"
if errorlevel 1 (
    echo Error: Could not create the ISO staging directory.
    exit /b 1
)
xcopy /e /i /q "out\uefi\esp\EFI" "%ISO_ROOT%\EFI" >nul
if errorlevel 1 (
    echo Error: Could not copy the UEFI bootloader into the ISO staging directory.
    exit /b 1
)
copy /y "out\uefi\esp\kernel.elf" "%ISO_ROOT%\kernel.elf" >nul
if errorlevel 1 (
    echo Error: Could not copy the kernel into the ISO staging directory.
    exit /b 1
)

"%MSYS_BIN%\qemu-img.exe" create -f raw "%EFI_IMAGE%" 16M
if errorlevel 1 (
    echo Error: Could not create the EFI boot image.
    exit /b 1
)
"%MSYS_BIN%\mformat.exe" -i "%EFI_IMAGE%" -v ASTRAOS ::
if errorlevel 1 (
    echo Error: Could not format the EFI boot image.
    exit /b 1
)
"%MSYS_BIN%\mcopy.exe" -s -i "%CD%\%EFI_IMAGE%" "%CD%\%ISO_ROOT%\EFI" ::
if errorlevel 1 (
    echo Error: Could not copy the UEFI bootloader into the EFI boot image.
    exit /b 1
)
"%MSYS_BIN%\mcopy.exe" -i "%CD%\%EFI_IMAGE%" "%CD%\%ISO_ROOT%\kernel.elf" ::
if errorlevel 1 (
    echo Error: Could not copy the kernel into the EFI boot image.
    exit /b 1
)

"%XORRISO%" -as mkisofs -R -J -V ASTRAOS -o "%ISO_PATH%" ^
    -eltorito-alt-boot -eltorito-platform efi ^
    -eltorito-boot efiboot.img -no-emul-boot "%ISO_ROOT%"
if errorlevel 1 (
    echo Error: Could not create the bootable ISO.
    exit /b 1
)
if exist "%ISO_ROOT%" rmdir /s /q "%ISO_ROOT%"
if errorlevel 1 (
    echo Error: Could not remove the ISO staging directory.
    exit /b 1
)
echo Created UEFI bootable ISO: %ISO_PATH%

copy /y "%OVMF_VARS_TEMPLATE%" "%OVMF_VARS_COPY%" >nul
if errorlevel 1 (
    echo Error: Could not copy the OVMF variable-store template.
    exit /b 1
)

qemu-system-x86_64 ^
    -machine q35,i8042=off ^
    -m 512M ^
    -device qemu-xhci ^
    -device usb-kbd ^
    -drive "if=pflash,format=raw,readonly=on,file=%OVMF_CODE%" ^
    -drive "if=pflash,format=raw,file=%OVMF_VARS_COPY%" ^
    -cdrom "%CD%\%ISO_PATH%" ^
    -boot order=d ^
    -serial stdio ^
    -no-reboot
if errorlevel 1 (
    echo Error: QEMU failed to run the ISO.
    exit /b 1
)
exit /b 0

:build_failed
echo Error: The AstraOS build failed.
exit /b 1
