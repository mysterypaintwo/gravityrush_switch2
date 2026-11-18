// probe_resume(const ThreadExceptionDump *d): continue the interrupted thread from d.
// libounce's handler path ends in svcBreak if __libnx_exception_handler returns, so a recovering
// handler has to restore the context itself. Every register is restored except x16 (IP0), which
// carries the branch target. Faulting code must therefore not keep a live value in x16 across the
// faulting instruction. The port's dynarec has to keep that register free in the same way.
// Offsets in ThreadExceptionDump: x0 at 16, fp 248, lr 256, sp 264, pc 272, q0 288, pstate 800.
    .text
    .global probe_resume
    .type probe_resume, %function
probe_resume:
    mov     x16, x0
    ldp     q0, q1, [x16, #288]
    ldp     q2, q3, [x16, #320]
    ldp     q4, q5, [x16, #352]
    ldp     q6, q7, [x16, #384]
    ldp     q8, q9, [x16, #416]
    ldp     q10, q11, [x16, #448]
    ldp     q12, q13, [x16, #480]
    ldp     q14, q15, [x16, #512]
    ldp     q16, q17, [x16, #544]
    ldp     q18, q19, [x16, #576]
    ldp     q20, q21, [x16, #608]
    ldp     q22, q23, [x16, #640]
    ldp     q24, q25, [x16, #672]
    ldp     q26, q27, [x16, #704]
    ldp     q28, q29, [x16, #736]
    ldp     q30, q31, [x16, #768]
    ldr     w17, [x16, #800]
    msr     nzcv, x17
    ldr     x17, [x16, #264]
    mov     sp, x17
    ldp     x0, x1, [x16, #16]
    ldp     x2, x3, [x16, #32]
    ldp     x4, x5, [x16, #48]
    ldp     x6, x7, [x16, #64]
    ldp     x8, x9, [x16, #80]
    ldp     x10, x11, [x16, #96]
    ldp     x12, x13, [x16, #112]
    ldp     x14, x15, [x16, #128]
    ldp     x18, x19, [x16, #160]
    ldp     x20, x21, [x16, #176]
    ldp     x22, x23, [x16, #192]
    ldp     x24, x25, [x16, #208]
    ldp     x26, x27, [x16, #224]
    ldr     x28, [x16, #240]
    ldp     x29, x30, [x16, #248]
    ldr     x17, [x16, #152]
    ldr     x16, [x16, #272]
    br      x16
    .size probe_resume, . - probe_resume
