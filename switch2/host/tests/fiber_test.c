/* Fibers under Box64 (gr_fiber_arm64.c): guest code (fiber_guest.c, built for x86-64) starts a
 * fiber, gets values back through ReturnToThread, resumes it, and switches to a second fiber
 * from inside the first. Exit status 0 when the result is right. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "box64e.h"
#include "fiber_guest.h"

int32_t gr_fiber_initialize(void *guest, const char *name, uintptr_t entry, uint64_t arg_init,
                            void *addr_context, uint64_t size_context, const void *opt, uint32_t flags, uint32_t build);
int32_t gr_fiber_run(void *guest, uint64_t arg, uint64_t *out_return);
int32_t gr_fiber_switch(void *guest, uint64_t arg, uint64_t *out_on_run);
int32_t gr_fiber_return_to_thread(uint64_t arg, uint64_t *out_on_run);

/* gr_fiber_arm64.c only needs this without a context buffer. */
void *runtime_low_map(size_t size, int prot) { return mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0); }

typedef struct {
    uintptr_t init, run, ret, swtch;
    void *fiber[2];
    void *ctx[2];
    uint64_t ctx_size;
} Api;

int main(void)
{
    box64e_jit jit;
    if (box64e_jit_create(32 << 20, &jit) || box64e_init(&jit)) { puts("init: FAIL"); return 1; }
    size_t size = 1 << 16;
    uint8_t *code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memcpy(code, fiber_guest, sizeof fiber_guest);
    box64e_set_protection((uintptr_t)code, size, PROT_READ | PROT_EXEC);

    static uint64_t fiber_buffers[2][16] __attribute__((aligned(16)));
    Api *api = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    api->init = box64e_bridge((void *)gr_fiber_initialize, BOX64E_STACK, "_sceFiberInitializeImpl");
    api->run = box64e_bridge((void *)gr_fiber_run, BOX64E_INT, "sceFiberRun");
    api->ret = box64e_bridge((void *)gr_fiber_return_to_thread, BOX64E_INT, "sceFiberReturnToThread");
    api->swtch = box64e_bridge((void *)gr_fiber_switch, BOX64E_INT, "sceFiberSwitch");
    api->fiber[0] = fiber_buffers[0];
    api->fiber[1] = fiber_buffers[1];
    api->ctx_size = 64 * 1024;
    api->ctx[0] = mmap(NULL, api->ctx_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    api->ctx[1] = mmap(NULL, api->ctx_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    uint64_t got = box64e_call_guest((uintptr_t)code + fiber_guest_fiber_test_main, 1, (uint64_t)(uintptr_t)api);
    printf("fiber test: %llu (want 7066) %s, guest error %d\n", (unsigned long long)got, got == 7066 ? "ok" : "FAIL",
           box64e_guest_error());
    return got == 7066 && !box64e_guest_error() ? 0 : 1;
}
