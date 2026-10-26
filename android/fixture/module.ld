/* Layout of a synthetic Switch module (fixture): text at 0 starting with the MOD0 pointer, then rodata, then data,
   each segment 16 KiB aligned (the loader seals in 16 KiB units; real modules are large enough that this holds). */
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.crt0)) *(.text .text.*) *(.plt) }
  . = ALIGN(0x4000);
  HIDDEN(__ro_start = .);
  .rodata : { KEEP(*(.rodata.mod0)) *(.rodata .rodata.*) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  .rela.dyn : { *(.rela.dyn) }
  .rela.plt : { *(.rela.plt) }
  .eh_frame_hdr : { HIDDEN(__eh_frame_hdr_start = .); *(.eh_frame_hdr) HIDDEN(__eh_frame_hdr_end = .); }
  .eh_frame : { KEEP(*(.eh_frame)) }
  . = ALIGN(0x4000);
  HIDDEN(__data_start = .);
  .dynamic : { *(.dynamic) }
  .got : { *(.got) }
  .got.plt : { *(.got.plt) }
  .data : { *(.data .data.*) }
  HIDDEN(__bss_start = .);
  .bss : { *(.bss .bss.*) *(COMMON) }
  HIDDEN(__bss_end = .);
  /DISCARD/ : { *(.note*) *(.comment) *(.interp) }
}
