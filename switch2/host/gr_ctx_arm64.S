// Host stack switching for guest fibers (gr_fiber_arm64.c), AAPCS64: macOS and Horizon.
// gr_ctx_t: x19-x30, sp, d8-d15 (168 bytes); everything else is caller-saved.
#ifdef __APPLE__
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

    .text
    .p2align 4
    .globl SYM(gr_ctx_switch)
// void gr_ctx_switch(gr_ctx_t* save, const gr_ctx_t* load)
SYM(gr_ctx_switch):
    mov     x9, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    str     x9,       [x0, #96]
    stp     d8,  d9,  [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9,       [x1, #96]
    ldp     d8,  d9,  [x1, #104]
    ldp     d10, d11, [x1, #120]
    ldp     d12, d13, [x1, #136]
    ldp     d14, d15, [x1, #152]
    mov     sp, x9
    ret

    .p2align 4
    .globl SYM(gr_ctx_entry)
// First code on a new stack (gr_ctx_init puts the function in x19, its argument in x20).
SYM(gr_ctx_entry):
    mov     x29, xzr
    mov     x0, x20
    blr     x19
    brk     #0          // the function must never return
