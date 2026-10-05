/* Linkitä ydin x86-64:n ELF-muotoon ja merkitse sen aloituskohta. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_main)

/* Erota suoritettava koodi kirjoitettavasta datasta. */
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* Ydin ladataan tähän fyysiseen osoitteeseen. */
    . = 0x100000;

    /* Ohjelmakoodi ja vain luku -merkkijonot. */
    .text ALIGN(0x1000) :
    {
        KEEP(*(.text.kernel_main))
        *(.text .text.*)
    } :text

    .rodata ALIGN(0x1000) :
    {
        *(.rodata .rodata.*)
    } :text

    /* Muuttujat ja nollattava BSS-muisti. */
    .data ALIGN(0x1000) :
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(0x1000) :
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /* Älä sisällytä ytimen suoritukseen liittymätöntä metadataa. */
    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
