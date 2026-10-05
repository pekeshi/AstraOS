; BIOS-kokeilu: tämä käynnistyssektori ei ole UEFI-ohjelma.
[org 0x7c00]

; Tyhjennä tekstinäyttö BIOSin videopalvelulla.
mov ah, 0x06
mov al, 0x00
mov bh, 0x07
mov ch, 0x00
mov cl, 0x00
mov dh, 0x18
mov dl, 0x4F
int 10h

; Tulosta viesti yksi merkki kerrallaan.
mov bx, hello_bios
mov ah, 0x0e

loop:
    mov al, [bx]
    cmp al, 0
    je exit
    int 0x10
    inc bx
    jmp loop

; Pysähdy, kun viesti on tulostettu.
exit:
    jmp $

; Nollaan päättyvä tulostettava viesti.
hello_bios db 'Bootloader working!', 0

; BIOS-käynnistyssektorin koko on 512 tavua.
times 510-($-$$) db 0

; BIOS tunnistaa käynnistyssektorin tästä allekirjoituksesta.
dw 0xAA55