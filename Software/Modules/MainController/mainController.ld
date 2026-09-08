/*************************************************************
* Created by J. Weij
*
* Linker script -- MainController application image.
* Flash: 52 KB at 0x08002000 (above the 8 KB bootloader). The image
* descriptor sits at a fixed 0xC0 offset, right after the vector table.
* See Spec/Node-Flash-Layout-and-Bootloader-Spec.md §3/§4.
*************************************************************/

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 52K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 8K
}

_estack         = ORIGIN(RAM) + LENGTH(RAM);
_Min_Stack_Size = 0x400;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        /* Image descriptor at a fixed offset the bootloader knows (0xC0). */
        . = 0xC0;
        KEEP(*(.image_descriptor))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text) *(.text*)
        *(.rodata) *(.rodata*)
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH
    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data) *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss) *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
