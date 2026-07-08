    .section .rodata.embedded_models,"a",%progbits
    .balign 16

    .balign 16
    .global _container_address_start
_container_address_start:
    .incbin "container.bin"
    .global _container_address_end
_container_address_end:
