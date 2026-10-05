# AstraOS

A small x86_64 UEFI operating system project.

## x86_64 UEFI bootloader

The BIOS boot sector in `src/boot.asm` is kept as a separate experiment.
`src/uefi.asm` is the original minimal assembly prototype. The current UEFI
application is built from `src/uefi_loader.c` and starts at
`EFI/BOOT/BOOTX64.EFI`. It opens `kernel.elf` from the EFI partition, checks and
loads its ELF64 segments, exits UEFI boot services, and transfers control to
the kernel.

### Build

Install [NASM](https://www.nasm.us/) and the x86_64-elf GCC toolchain. The
existing MinGW GCC targets Windows and cannot link the ELF64 kernel. Download
`x86_64-elf-tools-windows.zip` from the
[15.2.0 release](https://github.com/lordmilko/i686-elf-tools/releases/tag/15.2.0)
and extract it to `%USERPROFILE%\Tools\x86_64-elf-tools-15.2.0`. Keep the
toolchain's `bin` folder and its sibling folders together. The build expects
the compiler at `%USERPROFILE%\Tools\x86_64-elf-tools-15.2.0\bin`.

Run this from Command Prompt:

```bat
build.bat
```

The EFI application is placed under `out\uefi\esp\EFI\BOOT`. The freestanding
C kernel is built as `out\kernel\kernel.elf` and copied to the root of the EFI
partition as `out\uefi\esp\kernel.elf`.

### Run in QEMU

Install QEMU with OVMF firmware files, then run:

```bat
run.bat
```

`build.bat` builds the UEFI application and ELF64 kernel. `run.bat` rebuilds
them and launches QEMU/OVMF. The UEFI application prints a loading message;
after handoff, the C kernel prints `Hello from the AstraOS C kernel.` to COM1
and draws a top bar labeled `AstraOS Kernel - 0.1.0 - pekeshi` in the QEMU
window. The run script uses the MSYS2 firmware files
`C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd` and
`C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd`, so adjust those paths in
`run.bat` if your firmware is installed elsewhere. The variable-store template
is copied to `out\uefi` before each run. The UEFI console shows the loading
message, and the QEMU terminal shows the kernel's COM1 output. The kernel then
halts; close the QEMU window to stop it.

### Next milestones

The loader passes a `boot_info` structure from `src/boot_info.h`, including the
UEFI memory map and framebuffer address, dimensions, stride, and pixel format.
The kernel starts on its own stack. Build on that foundation:

1. Walk the memory-map descriptors and report usable memory from the kernel.
2. Add a kernel-owned physical page allocator using the memory map.
3. Set up the GDT, IDT, and exception handlers so faults can be diagnosed.
4. Add a timer and keyboard input after the basic memory and exception setup.

After `ExitBootServices`, the kernel must not call UEFI boot services. Keep the
kernel freestanding: it has no C library or normal operating system to rely on.