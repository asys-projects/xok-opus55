/*
 * Linker script for ExOS programs: read-only text (shared from the
 * buffer cache by exec) and writable data in separate segments.
 */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS (5);
  data PT_LOAD FLAGS (6);
}

SECTIONS
{
  . = 0x00800000 + SIZEOF_HEADERS;
  .text : {
    *(.text.start)
    *(.text .text.*)
  } :text
  .rodata : {
    *(.rodata .rodata.*)
  } :text
  . = ALIGN(4096);
  .init_array : {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  } :data
  .data : {
    *(.data .data.*)
  } :data
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  } :data
  . = ALIGN(4096);
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
