/* Fixture "subsdk0": calls into sdk through its PLT, and reads x18. */
#define FN(n) .global n; .type n, %function; .balign 16; n: .cfi_startproc
#define END(n) .cfi_endproc; .size n, . - n
.text
FN(fx_sub_x18_plus_tick)  /* x0 = x18 + (tick != 0) */
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _ZN2nn2os13GetSystemTickEv
    cmp x0, #0
    cset x0, ne
    add x0, x0, x18
    ldp x29, x30, [sp], #16
    ret
END(fx_sub_x18_plus_tick)
