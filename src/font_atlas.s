    .section .rodata.iz_font,"a",%progbits
    .balign 8
    .globl iz_font
iz_font:
    .incbin "nup-library-atlas.bin"
    .section .rodata.iz_brand,"a",%progbits
    .balign 8
    .globl iz_brand, iz_brand_end
iz_brand:
    .incbin "nup-library-icon-70.png"
iz_brand_end:
    .globl iz_brand_compact, iz_brand_compact_end
iz_brand_compact:
    .incbin "nup-library-icon-47.png"
iz_brand_compact_end:
    .section .note.GNU-stack,"",%progbits
