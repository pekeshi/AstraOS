[org 0x7c00] ; The origin, the bootloader's entrypoint.

; Clear the BIOS VGA memory, (clearing the screen).
mov ah, 0x06    ; Scroll up / Clear window function
mov al, 0x00    ; Number of lines to scroll (0 = clear entire window)
mov bh, 0x07    ; Attribute byte (white text on a black background)
mov ch, 0x00    ; Upper-left row
mov cl, 0x00    ; Upper-left column
mov dh, 0x18    ; Lower-right row (24 decimal / 0x18)
mov dl, 0x4F    ; Lower-right column (79 decimal / 0x4F)
int 10h         ; Call BIOS video interrupt

; Move the cursor to the top-left row W.I.P


; Store the message from hello_bios to the bx register
mov bx, hello_bios  ; Move the message string into bx register
mov ah, 0x0e        ; Call the BIOS teletype output mode

loop:               ; Loop the string until we find 0, then jump into exit
    mov al, [bx]    ; Move the character from bx to al
    cmp al, 0       ; Compare character from al to 0
    je exit         ; If equals (al = 0), jump to exit
    int 0x10        ; Call the print interrupt
    inc bx          ; Increment bx, (to get the other character)
    jmp loop        ; Jump to the start, (now we have a loop that runs until we get a 0)

exit:               ; The exit loop
    jmp $           ; Just jumps to the start

hello_bios db 'Bootloader working!', 0  ; Our null-terminated message string

times 510-($-$$) db 0                   ; Padding of 0'x so that our bootloader is full
dw 0xAA55