; 64-bittinen UEFI-ohjelma: tulostaa viestin ja pysähtyy.
BITS 64
ORG 0

; Tiedosto ja koodiosio kohdistetaan UEFI:n vaatimalla tavalla.
%define FILE_ALIGNMENT                  0x200       ; 0x200 = 512 tavua
%define SECTION_ALIGNMENT               0x1000      ; 0x1000 = 4096 tavua
%define IMAGE_BASE                      0x10000000  ; Image base address for UEFI applications
%define IMAGE_FILE_RELOCS_STRIPPED      0x0001      ; No relocation information is present in the file., no need to perform any fixups.
%define IMAGE_FILE_EXECUTABLE_IMAGE     0x0002      ; The file is executable (no unresolved external references).
%define IMAGE_FILE_LARGE_ADDRESS_AWARE  0x0020      ; The application can handle addresses larger than 2 GB.
%define IMAGE_SCN_CNT_CODE              0x00000020  ; The section contains executable code.
%define IMAGE_SCN_MEM_EXECUTE           0x20000000  ; The section can be executed as code.
%define IMAGE_SCN_MEM_READ              0x40000000  ; The section can be read.

; Koodiosion koko pyöristetään tiedoston kohdistukseen.
%define aligned_code_size ((code_end - code_start + FILE_ALIGNMENT - 1) & ~(FILE_ALIGNMENT - 1))

; MZ-otsake osoittaa PE-otsakkeen sijainnin.
dos_header:
    dw 0x5A4D
    times 0x3A db 0
    dd pe_header

; PE-otsake kertoo, että ohjelma on x86-64-kuva.
pe_header:
    db 'PE', 0, 0
    dw 0x8664
    dw 1
    dd 0
    dd 0
    dd 0
    dw 0xF0
    dw IMAGE_FILE_RELOCS_STRIPPED | IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE

; PE32+-otsake määrittelee käynnistyskohdan ja muistiasettelun.
optional_header:
    dw 0x20B
    db 0, 0
    dd aligned_code_size
    dd 0
    dd 0
    dd 0x1000 + (efi_entry - code_start)
    dd 0x1000
    dq IMAGE_BASE
    dd SECTION_ALIGNMENT
    dd FILE_ALIGNMENT
    dw 0, 0
    dw 0, 0
    dw 0, 0
    dd 0
    dd 0x1000 + ((code_end - code_start + SECTION_ALIGNMENT - 1) & ~(SECTION_ALIGNMENT - 1))
    dd FILE_ALIGNMENT
    dd 0
    dw 10
    dw 0x0100
    dq 0x100000
    dq 0x1000
    dq 0x100000
    dq 0x1000
    dd 0
    dd 16
    times 16 * 8 db 0

; Ainoa osio sisältää suoritettavan koodin.
section_header:
    db '.text', 0, 0, 0
    dd code_end - code_start
    dd 0x1000
    dd aligned_code_size
    dd FILE_ALIGNMENT
    dd 0
    dd 0
    dw 0
    dw 0
    dd IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ

; Otsake täytetään tiedoston kohdistusrajaan asti.
    times FILE_ALIGNMENT - ($ - $$) db 0

; UEFI aloittaa suorituksen tästä.
code_start:
efi_entry:
    sub rsp, 0x28
    mov rax, [rdx + 0x40]
    mov rcx, rax
    lea rdx, [rel boot_message]
    call qword [rax + 0x08]
    add rsp, 0x28

; Pysähdy tulostuksen jälkeen.
halt:
    cli
    hlt
    jmp halt

; UEFI:n tulostusviesti on UTF-16-muodossa.
boot_message:
    dw 'A', 's', 't', 'r', 'a', 'O', 'S', ' ', 'U', 'E', 'F', 'I', ' '
    dw 'b', 'o', 'o', 't', 'l', 'o', 'a', 'd', 'e', 'r', ' '
    dw 's', 't', 'a', 'r', 't', 'e', 'd', '.', 13, 10, 0

code_end:
    times aligned_code_size - (code_end - code_start) db 0
