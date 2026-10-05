BITS 64

section .text
global enter_kernel

; UEFI antaa argumentit rekistereissä RCX, RDX ja R8.
; Ydin käyttää System V -kutsukäytäntöä, jossa ensimmäinen argumentti on RDI.
enter_kernel:
    cli
    cld                 ; Merkkijonokäskyt etenevät muistissa eteenpäin.
    mov rsp, r8         ; Ota käyttöön ytimen oma pino.
    and rsp, -16        ; Kohdista pino 16 tavun rajalle.
    mov rdi, rdx        ; Välitä käynnistystiedot ytimen argumenttina.
    call rcx            ; Siirry ytimen aloituskohtaan.

.halt:
    ; Ytimen paluu ei ole sallittu: pysähdy turvallisesti.
    hlt
    jmp .halt
