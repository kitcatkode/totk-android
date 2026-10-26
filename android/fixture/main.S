/* Fixture "main": nnMain and a PLT call into sdk (the loader's cross-module probe calls the PLT stub). */
#define FN(n) .global n; .type n, %function; .balign 16; n: .cfi_startproc
#define END(n) .cfi_endproc; .size n, . - n
.text
FN(nnMain)
    ret
END(nnMain)

FN(fx_main_tick)
    b _ZN2nn2os13GetSystemTickEv
END(fx_main_tick)

FN(fx_main_x18)           /* x0 = x18 (read from main as well) */
    mov x0, x18
    ret
END(fx_main_x18)
