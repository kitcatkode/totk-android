/* Assembler differences between Mach-O (Darwin) and ELF (Linux / Android) for the runtime's .S files.
   SYM(x)        the C symbol x (Mach-O prefixes an underscore)
   PAGE(x)       adrp operand for symbol x
   PAGEOFF(x)    low-12-bit operand for symbol x (ldr / add after the adrp)
   FUNC(x)       start a global function x (ELF also records its type, for backtraces and the linker)
   GLOBL(x)      a global data symbol x
   CSTRING       switch to the section for NUL-terminated string literals
   host_slots r  macro: x<r> = this thread's slot base (host_tp.h; third_party/d2r/hostreg.py) */
#pragma once
#if defined(__APPLE__)
#define SYM(x) _##x
#define PAGE(x) x@PAGE
#define PAGEOFF(x) x@PAGEOFF
#define FUNC(x) .globl SYM(x)
#define GLOBL(x) .globl SYM(x)
#define CSTRING .cstring
#else
#define SYM(x) x
#define PAGE(x) x
#define PAGEOFF(x) :lo12:x
#define FUNC(x) .globl x; .type x, %function
#define GLOBL(x) .globl x
#define CSTRING .section .rodata.str1.1, "aMS", %progbits, 1
#endif

.macro host_slots reg
#if defined(__APPLE__)
    mrs \reg, tpidrro_el0
#else
    mrs \reg, tpidr_el0
    ldr \reg, [\reg, #16]
#endif
.endm
