/* libSceFiber on the ARM64 host (the PC port's src/gr_fiber.inc is built on Windows fibers).
 *
 * A guest fiber runs on the thread's Box64 emulator. Switching fibers swaps two things:
 * - the host stack (gr_ctx_arm64.S), which holds the dynarec and bridge frames of the fiber
 *   that is suspended inside its sceFiberSwitch/ReturnToThread call, together with the guest
 *   registers Box64 keeps in native callee-saved registers across a bridge;
 * - the emulator's guest CPU state (box64e_save_state/box64e_load_state).
 * The guest stack is the context buffer the game passes to sceFiberInitialize, as on the PS4. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "runtime.h"
#include "box64e.h"

#define GR_FIB_MAGIC 0x52424946u
#define GR_FIB_ERR(n) ((int32_t)(UINT32_C(0x80590000) | (n)))
/* 1 NULL, 2 ALIGNMENT, 3 RANGE, 4 INVALID, 5 PERMISSION, 6 STATE, 7 BUSY, 8 AGAIN, 9 FATAL */
#define HOST_STACK (1024 * 1024)
#define MIN_GUEST_STACK (16 * 1024)

typedef struct { uint64_t r[21]; } gr_ctx_t; /* x19-x30, sp, d8-d15 */
void gr_ctx_switch(gr_ctx_t* save, const gr_ctx_t* load);
void gr_ctx_entry(void);

typedef struct {
    gr_ctx_t ctx;
    void* state; /* guest CPU state while not running */
} Side;

typedef struct GrFiber {
    uint32_t magic;
    int state;               /* 0 not started, 1 running, 2 suspended, 3 finished */
    void* guest;             /* the buffer the game gave to sceFiberInitialize */
    uintptr_t entry;         /* guest void entry(uint64_t arg_init, uint64_t arg_run) */
    uint64_t arg_init, arg_run;
    uint64_t* out_return;    /* receives the value passed to ReturnToThread */
    uint64_t* resume_out;    /* receives the value when this fiber is resumed */
    uintptr_t guest_stack_top;
    void* host_stack;
    Side self;
    Side* return_to;         /* the thread that ran the fiber chain */
    char name[32];
} GrFiber;

static __thread GrFiber* current;
static __thread Side thread_side;

static GrFiber* get(void* guest)
{
    if (!guest) return NULL;
    uint64_t* g = guest;
    GrFiber* f = (GrFiber*)(uintptr_t)g[1];
    return (g[0] == GR_FIB_MAGIC && f && f->magic == GR_FIB_MAGIC) ? f : NULL;
}

static void* new_state(void) { return calloc(1, box64e_state_size()); }

/* Saves the running side, loads the target's guest state and switches host stacks. Returns
 * when something switches back to `from` (and has already loaded its state). */
static void switch_sides(Side* from, Side* to)
{
    x64emu_t* emu = box64e_thread_emu();
    box64e_save_state(emu, from->state);
    box64e_load_state(emu, to->state);
    gr_ctx_switch(&from->ctx, &to->ctx);
}

static __attribute__((noreturn)) void fiber_main(void* p)
{
    GrFiber* f = p;
    x64emu_t* emu = box64e_thread_emu();
    box64e_set_reg(emu, 4, f->guest_stack_top);
    uint64_t args[2] = {f->arg_init, f->arg_run};
    box64e_call_guest_args(f->entry, 2, args); /* normally never returns */
    /* it returned: treat as a final ReturnToThread(0) */
    f->state = 3;
    if (f->out_return) *f->out_return = 0;
    current = NULL;
    for (;;) switch_sides(&f->self, f->return_to);
}

/* The first switch to a fiber starts fiber_main on its host stack with the switching side's
 * guest state (fiber_main then moves rsp to the fiber's guest stack). */
static int start(GrFiber* f, uint64_t arg, const Side* from)
{
    f->arg_run = arg;
    if (!f->host_stack) {
        f->host_stack = mmap(NULL, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (f->host_stack == MAP_FAILED) { f->host_stack = NULL; return 0; }
    }
    memset(&f->self.ctx, 0, sizeof f->self.ctx);
    f->self.ctx.r[0] = (uint64_t)(uintptr_t)fiber_main;                 /* x19 */
    f->self.ctx.r[1] = (uint64_t)(uintptr_t)f;                          /* x20 */
    f->self.ctx.r[11] = (uint64_t)(uintptr_t)gr_ctx_entry;              /* x30 */
    f->self.ctx.r[12] = ((uint64_t)(uintptr_t)f->host_stack + HOST_STACK) & ~(uint64_t)15; /* sp */
    box64e_save_state(box64e_thread_emu(), f->self.state);              /* fiber_main starts from it */
    (void)from;
    return 1;
}

int32_t gr_fiber_initialize(void* guest, const char* name, uintptr_t entry, uint64_t arg_init,
                            void* addr_context, uint64_t size_context, const void* opt, uint32_t flags, uint32_t build)
{
    (void)opt; (void)flags; (void)build;
    if (!guest || !entry) return GR_FIB_ERR(1);
    if ((uintptr_t)guest & 15) return GR_FIB_ERR(2);
    GrFiber* f = calloc(1, sizeof *f);
    if (!f) return GR_FIB_ERR(9);
    f->self.state = new_state();
    f->magic = GR_FIB_MAGIC;
    f->guest = guest;
    f->entry = entry;
    f->arg_init = arg_init;
    if (addr_context && size_context >= MIN_GUEST_STACK) {
        f->guest_stack_top = ((uintptr_t)addr_context + size_context) & ~(uintptr_t)15;
    } else {
        /* no context buffer: a guest stack below 1 TiB like other guest stacks */
        size_t s = size_context > 256 * 1024 ? (size_t)size_context : 256 * 1024;
        void* stack = runtime_low_map(s, PROT_READ | PROT_WRITE);
        if (!stack) { free(f->self.state); free(f); return GR_FIB_ERR(9); }
        f->guest_stack_top = ((uintptr_t)stack + s) & ~(uintptr_t)15;
    }
    if (name) snprintf(f->name, sizeof f->name, "%s", name);
    uint64_t* g = guest;
    g[0] = GR_FIB_MAGIC;
    g[1] = (uint64_t)(uintptr_t)f;
    return 0;
}

int32_t gr_fiber_finalize(void* guest)
{
    GrFiber* f = get(guest);
    if (!f) return GR_FIB_ERR(1);
    if (f->state == 1) return GR_FIB_ERR(6);
    if (f->host_stack) munmap(f->host_stack, HOST_STACK);
    uint64_t* g = guest;
    g[0] = g[1] = 0;
    f->magic = 0;
    free(f->self.state);
    free(f);
    return 0;
}

int32_t gr_fiber_run(void* guest, uint64_t arg, uint64_t* out_return)
{
    GrFiber* f = get(guest);
    if (!f) return GR_FIB_ERR(1);
    if (current) return GR_FIB_ERR(5); /* Run belongs to a thread, not to a fiber */
    if (f->state == 1 || f->state == 3) return GR_FIB_ERR(6);
    if (!thread_side.state && !(thread_side.state = new_state())) return GR_FIB_ERR(9);
    f->return_to = &thread_side;
    f->out_return = out_return;
    if (f->state == 0) { if (!start(f, arg, &thread_side)) return GR_FIB_ERR(9); }
    else if (f->resume_out) *f->resume_out = arg;
    f->state = 1;
    current = f;
    switch_sides(&thread_side, &f->self);
    current = NULL; /* back on the thread */
    return 0;
}

int32_t gr_fiber_switch(void* guest, uint64_t arg, uint64_t* out_on_run)
{
    GrFiber *to = get(guest), *from = current;
    if (!to) return GR_FIB_ERR(1);
    if (!from) return GR_FIB_ERR(5);
    if (to == from || to->state == 1 || to->state == 3) return GR_FIB_ERR(6);
    to->return_to = from->return_to;
    to->out_return = from->out_return;
    if (to->state == 0) { if (!start(to, arg, &from->self)) return GR_FIB_ERR(9); }
    else if (to->resume_out) *to->resume_out = arg;
    from->resume_out = out_on_run;
    from->state = 2;
    to->state = 1;
    current = to;
    switch_sides(&from->self, &to->self);
    return 0; /* resumed: our value is already stored */
}

int32_t gr_fiber_return_to_thread(uint64_t arg, uint64_t* out_on_run)
{
    GrFiber* f = current;
    if (!f) return GR_FIB_ERR(5);
    f->resume_out = out_on_run;
    f->state = 2;
    if (f->out_return) *f->out_return = arg;
    current = NULL;
    switch_sides(&f->self, f->return_to);
    return 0;
}

int32_t gr_fiber_get_self(void** out)
{
    if (!out) return GR_FIB_ERR(1);
    if (!current) return GR_FIB_ERR(5);
    *out = current->guest;
    return 0;
}
