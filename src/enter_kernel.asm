BITS 64

section .text
global enter_kernel

; Microsoft x64 arguments: RCX=entry, RDX=boot info, R8=stack top.
; The kernel entry follows the System V x86_64 calling convention.
enter_kernel:
    cli
    cld
    mov rsp, r8
    and rsp, -16
    mov rdi, rdx
    call rcx

.halt:
    hlt
    jmp .halt
