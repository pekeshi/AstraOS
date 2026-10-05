@echo off
setlocal
cd /d "%~dp0"

where nasm >nul 2>nul
if errorlevel 1 (
    echo Error: NASM was not found. Install NASM and add it to PATH.
    exit /b 1
)

set "ELF_BIN=%USERPROFILE%\Tools\x86_64-elf-tools-15.2.0\bin"
if not exist "%ELF_BIN%\x86_64-elf-gcc.exe" (
    echo Error: x86_64-elf-gcc was not found in:
    echo %ELF_BIN%
    echo Install the x86_64-elf toolchain and try again.
    exit /b 1
)

set "MINGW_BIN=C:\msys64\mingw64\bin"
if not exist "%MINGW_BIN%\gcc.exe" (
    echo Error: MinGW GCC was not found in:
    echo %MINGW_BIN%
    exit /b 1
)

if not exist "out\uefi\esp\EFI\BOOT" mkdir "out\uefi\esp\EFI\BOOT"
if errorlevel 1 (
    echo Error: Could not create the UEFI output directory.
    exit /b 1
)

if not exist "out\kernel" mkdir "out\kernel"
if errorlevel 1 (
    echo Error: Could not create the kernel output directory.
    exit /b 1
)

nasm -f win64 "src\enter_kernel.asm" -o "out\kernel\enter_kernel.obj"
if errorlevel 1 (
    echo Error: NASM failed to assemble the kernel handoff routine.
    exit /b 1
)

"%ELF_BIN%\x86_64-elf-gcc.exe" -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mcmodel=small -c "src\kernel.c" -o "out\kernel\kernel.o"
if errorlevel 1 (
    echo Error: Failed to compile the freestanding C kernel.
    exit /b 1
)

"%ELF_BIN%\x86_64-elf-gcc.exe" -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mcmodel=small -c "src\pmm.c" -o "out\kernel\pmm.o"
if errorlevel 1 (
    echo Error: Failed to compile the physical memory manager.
    exit /b 1
)

"%ELF_BIN%\x86_64-elf-gcc.exe" -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mcmodel=small -c "src\acpi.c" -o "out\kernel\acpi.o"
if errorlevel 1 (
    echo Error: Failed to compile ACPI shutdown support.
    exit /b 1
)

"%ELF_BIN%\x86_64-elf-gcc.exe" -nostdlib -no-pie -Wl,-T,src\kernel.ld -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -o "out\kernel\kernel.elf" "out\kernel\kernel.o" "out\kernel\pmm.o" "out\kernel\acpi.o"
if errorlevel 1 (
    echo Error: Failed to link the ELF64 kernel.
    exit /b 1
)

"%MINGW_BIN%\gcc.exe" -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -mno-red-zone -c "src\uefi_loader.c" -o "out\kernel\uefi_loader.o"
if errorlevel 1 (
    echo Error: Failed to compile the UEFI loader.
    exit /b 1
)

"%MINGW_BIN%\gcc.exe" -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--image-base,0x10000000 -Wl,--file-alignment,512 -Wl,--section-alignment,4096 -Wl,--dynamicbase -Wl,--nxcompat -o "out\uefi\esp\EFI\BOOT\BOOTX64.EFI" "out\kernel\uefi_loader.o" "out\kernel\enter_kernel.obj"
if errorlevel 1 (
    echo Error: Failed to link the x86_64 UEFI application.
    exit /b 1
)

copy /y "out\kernel\kernel.elf" "out\uefi\esp\kernel.elf" >nul
if errorlevel 1 (
    echo Error: Could not copy the kernel to the EFI partition.
    exit /b 1
)

echo Built UEFI application: out\uefi\esp\EFI\BOOT\BOOTX64.EFI
echo Built freestanding ELF64 kernel: out\kernel\kernel.elf
echo Copied kernel to: out\uefi\esp\kernel.elf
exit /b 0
