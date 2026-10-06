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
after handoff, the C kernel prints `Hello from the AstraOS C kernel.` to COM1,
draws a top bar labeled `AstraOS Kernel - 0.1.0 - pekeshi` in the QEMU window,
and starts a visible kernel shell. Click the QEMU window and use its keyboard,
or type into the terminal running `run.bat` over COM1.
The commands are `help`, `about`, `clear`, `mem`, `alloc`,
`free <hex-address>`, and `shutdown`. `mem` reports currently free physical
pages, `alloc` reserves one page and prints its physical address, and `free`
releases a page previously returned by `alloc`. The physical allocator tracks
memory below 64 GiB; pages outside that range are not managed. Shell output is
shown in the QEMU framebuffer and mirrored to COM1. `shutdown` requests ACPI S5
soft-off using the tables provided by firmware; soft-off depends on firmware
and hardware support, and unsupported ACPI configurations are reported. The
run script uses the
MSYS2 firmware files
`C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd` and
`C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd`, so adjust those paths in
`run.bat` if your firmware is installed elsewhere. The variable-store template
is copied to `out\uefi` before each run. The UEFI console shows the loading
message. Close the QEMU window to stop it.

To create `out\AstraOS.iso` and boot it from QEMU, install xorriso and MSYS2
mtools, then run `run-iso.bat`.

### Next milestones

The loader passes a `boot_info` structure from `src/boot_info.h`, including the
UEFI memory map and framebuffer address, dimensions, stride, and pixel format.
The kernel starts on its own stack. Build on that foundation:

1. Add GDT, IDT, and exception handlers so faults can be diagnosed.
2. Add a timer and interrupt-driven input, then extend USB support for hubs.

After `ExitBootServices`, the kernel must not call UEFI boot services. Keep the
kernel freestanding: it has no C library or normal operating system to rely on.

The kernel first checks the standard 8042/PS/2-compatible keyboard interface,
enables its keyboard port and scan-code translation, and requires a keyboard
acknowledgement before treating it as available. This prevents a controller
that is present without a keyboard from blocking USB discovery, while retaining
firmware legacy keyboard emulation when it responds. If PS/2 initializes but
produces no input, the kernel polls the USB path as well. USB uses a polled
xHCI driver for HID boot-protocol keyboards connected directly to an xHCI root
port; the kernel scans each PCI xHCI controller until it finds a keyboard.
When no keyboard is attached, or the active keyboard is unplugged, the kernel
retries discovery about once per second, so a keyboard can be moved between
directly connected root ports without rebooting. Taking ownership of xHCI may
disable firmware keyboard emulation. The xHCI driver does not yet support USB
hubs, non-boot HID keyboards, or controllers whose MMIO BAR is not
identity-mapped by the firmware. COM1 input remains available as a fallback.
The xHCI controller and DMA buffers are accessed through the firmware's current
physical identity mappings; the kernel does not yet manage its own page tables.

The PS/2-compatible path enables the first keyboard port and scan-code
translation, confirms the keyboard responds, and polls for US set-1 scan codes
with Shift and Caps Lock support. PS/2 is preferred when it returns a character;
USB is initialized at startup and polled as a fallback.

If keyboard discovery fails, the kernel prints xHCI diagnostics: the number of
controllers scanned and with connected ports, the relevant controller's PCI
location and initialization stage, root-port/connection/reset counts, the last
relevant port's PORTSC value, speed, and reset stage, and the USB enumeration
stage. It also lists every root port on the selected controller with its raw
PORTSC value, reset outcome, and (for connected ports) the USB enumeration
stage and last command completion. Controller stages identify BAR validation, PCI setup, capability
discovery, firmware handoff, reset, DMA allocation, and controller start.
Port stages distinguish no connection, debounce, connection loss, reset
timeout, and link stabilization timeout. The xHCI driver waits for ports to
settle after controller start, including ports that firmware had already
powered. Enumeration stages 1-13 identify,
in order: enable slot, allocate device buffers, address device, read the device
descriptor header, read the complete device descriptor, read the configuration
descriptor, validate configuration data, find a boot-keyboard interface, find
its interrupt-IN endpoint, set configuration and boot protocol, allocate the
interrupt ring, validate the endpoint interval, and configure the interrupt
endpoint. The last command type and xHCI completion code are also printed.
Stage 0 means no connected port completed reset and began enumeration.

The QEMU run scripts disable the emulated PS/2 controller and attach an
emulated xHCI controller with a USB keyboard, so typing in the QEMU display
exercises the kernel's USB path directly. COM1 input remains available
throughout. On physical hardware, connect a boot-protocol USB keyboard directly
to an xHCI root port; USB hubs are not supported yet. The kernel initializes
both available keyboard paths at startup, preferring PS/2 input and polling
xHCI when PS/2 has no character to return.