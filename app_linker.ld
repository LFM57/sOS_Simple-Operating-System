ENTRY(_start)

SECTIONS
{
    /* Default virtual loading address */
    . = 0x8000000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    /* BSS section is managed dynamically by our loader */
    .bss : {
        *(.bss)
    }
}