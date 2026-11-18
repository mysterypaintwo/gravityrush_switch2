// The NRO's exception entry: crt0 branches here when the process entry receives a user-mode
// exception (hbloader forwards it, x0 = type, x1 = kernel context). Before libounce's own entry
// runs, this sleeps for 5 seconds once, so the delay before the error screen shows whether the
// exception reached this NRO at all. Only x0-x8 are scratch here (the kernel saved them in the
// context, and libounce's entry treats them the same way). x9 and above are still the
// interrupted thread's.
    .text
    .global __libnx_exception_entry
    .type __libnx_exception_entry, %function
__libnx_exception_entry:
    adrp    x2, exc_stub_save
    add     x2, x2, :lo12:exc_stub_save
    stp     x0, x1, [x2]
    ldr     w3, [x2, #16]
    cbnz    w3, 1f
    mov     w3, #1
    str     w3, [x2, #16]
    movz    x0, #0xf200
    movk    x0, #0x2a05, lsl #16     // 5,000,000,000 ns = 0x12a05f200
    movk    x0, #0x1, lsl #32
    svc     0xb                      // svcSleepThread
1:  adrp    x2, exc_stub_save
    add     x2, x2, :lo12:exc_stub_save
    ldp     x0, x1, [x2]
    b       __libounce_exception_entry
    .size __libnx_exception_entry, . - __libnx_exception_entry

    .bss
    .align 3
    .global exc_stub_save
exc_stub_save:
    .skip 24                         // x0, x1, entered flag
