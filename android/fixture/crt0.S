/* Module header shared by the fixture modules: word 1 of text is the offset of MOD0. */
.section .text.crt0, "ax"
.global __module_start
__module_start:
    b 1f
    .word __nx_mod0 - __module_start
1:  ret

.section .rodata.mod0, "a"
.balign 4
__nx_mod0:
    .ascii "MOD0"
    .word _DYNAMIC - __nx_mod0
    .word __bss_start - __nx_mod0
    .word __bss_end - __nx_mod0
    .word __eh_frame_hdr_start - __nx_mod0
    .word __eh_frame_hdr_end - __nx_mod0
    .word 0
