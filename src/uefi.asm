; 64-bittinen UEFI-ohjelma: tulostaa viestin ja pysähtyy.
BITS 64
ORG 0

; Tiedoston ja muistiosion kohdistukset.
%define FILE_ALIGNMENT                  0x200       ; 512 tavua tiedostossa.
%define SECTION_ALIGNMENT               0x1000      ; 4096 tavua muistissa.
%define IMAGE_BASE                      0x10000000  ; Ohjelman latauksen perusosoite.
%define IMAGE_FILE_RELOCS_STRIPPED      0x0001      ; Tiedostossa ei ole korjaustietoja.
%define IMAGE_FILE_EXECUTABLE_IMAGE     0x0002      ; Tiedosto on suoritettava ohjelma.
%define IMAGE_FILE_LARGE_ADDRESS_AWARE  0x0020      ; Ohjelma voi käyttää yli 2 Gt:n osoitteita.
%define IMAGE_SCN_CNT_CODE              0x00000020  ; Osio sisältää konekoodia.
%define IMAGE_SCN_MEM_EXECUTE           0x20000000  ; Osion koodi voidaan suorittaa.
%define IMAGE_SCN_MEM_READ              0x40000000  ; Osiota voidaan lukea.

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

; PE32+-otsake määrittelee aloituskohdan, kohdistukset ja UEFI-tyypin.
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

; Ainoa osio sisältää suoritettavan koodin ja viestin.
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
