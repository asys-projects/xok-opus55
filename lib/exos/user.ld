/*
 * Linker script for ExOS programs.
 */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x00800000;
  .text : {
    *(.text.start)
    *(.text .text.*)
  }
  .rodata : {
    *(.rodata .rodata.*)
  }
  .init_array : {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  }
  . = ALIGN(4096);
  .data : {
    *(.data .data.*)
  }
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  }
  . = ALIGN(4096);
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
