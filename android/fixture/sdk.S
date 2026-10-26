/* Fixture "sdk": every kind of instruction the rewriter has to replace, one small function each. Guest semantics:
   x18 is the guest's own register, TPIDR_EL0 the guest TLS pointer, TPIDRRO_EL0 the Horizon thread-local region. */
#define FN(n) .global n; .type n, %function; .balign 16; n: .cfi_startproc
#define END(n) .cfi_endproc; .size n, . - n
.text
FN(_ZN2nn2os13GetSystemTickEv)
    mrs x0, cntpct_el0
    ret
END(_ZN2nn2os13GetSystemTickEv)

FN(fx_x18_set)            /* x18 = x0 */
    mov x18, x0
    ret
END(fx_x18_set)

FN(fx_x18_add)            /* x0 = x18 + x0 */
    add x0, x18, x0
    ret
END(fx_x18_add)

FN(fx_x18_load)           /* x0 = *(x18 + 8) */
    ldr x0, [x18, #8]
    ret
END(fx_x18_load)

FN(fx_x18_cbz)            /* x0 = x18 ? 1 : 2 */
    cbz x18, 1f
    mov x0, #1
    ret
1:  mov x0, #2
    ret
END(fx_x18_cbz)

FN(fx_x18_adr)            /* x18 = address of the next instruction; x0 = x18 */
    adr x18, 1f
1:  mov x0, x18
    ret
END(fx_x18_adr)

FN(fx_x18_atomic_inc)     /* atomically ++*(uint64_t *)x18, x0 = new value (exclusive window through x18) */
1:  ldxr x0, [x18]
    add x0, x0, #1
    stxr w1, x0, [x18]
    cbnz w1, 1b
    ret
END(fx_x18_atomic_inc)

FN(fx_x18_call)           /* x0 = ((fn)x18)(x0, x1): a BLR x18 call window after a movable stack store */
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x1, [sp, #16]
    blr x18
    ldp x29, x30, [sp], #32
    ret
END(fx_x18_call)

FN(fx_tls_get)
    mrs x0, tpidr_el0
    ret
END(fx_tls_get)

FN(fx_tls_set)
    msr tpidr_el0, x0
    ret
END(fx_tls_set)

FN(fx_tlr_get)
    mrs x0, tpidrro_el0
    ret
END(fx_tlr_get)

FN(fx_svc)                /* svc #0x7e: the host stub returns x0 * 3 + 0x7e */
    svc #0x7e
    ret
END(fx_svc)

FN(fx_regs_survive)       /* the gate keeps every register but its target: x19 + x28 + d8 bits across a timer read */
    stp x29, x30, [sp, #-48]!
    stp x19, x28, [sp, #16]
    str d8, [sp, #32]
    mov x19, x0
    mov x28, x1
    fmov d8, x2
    mrs x3, cntvct_el0
    fmov x4, d8
    add x0, x19, x28
    add x0, x0, x4
    ldr d8, [sp, #32]
    ldp x19, x28, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
END(fx_regs_survive)

.data
.balign 8
.global sdk_nnmain_ref
sdk_nnmain_ref: .quad nnMain    /* an ABS64 import: the loader's guest-address identity check */
