/* Guest (x86-64) side of fiber_test.c: compiled for x86-64 and run under Box64. No globals or
 * strings: everything comes through the api pointer, so the code needs no relocations. */
#include <stdint.h>

typedef struct {
    int32_t (*init)(void *f, const char *name, void (*entry)(uint64_t, uint64_t), uint64_t arg_init,
                    void *ctx, uint64_t ctx_size, const void *opt, uint32_t flags, uint32_t build);
    int32_t (*run)(void *f, uint64_t arg, uint64_t *out);
    int32_t (*ret)(uint64_t arg, uint64_t *out);
    int32_t (*swtch)(void *f, uint64_t arg, uint64_t *out);
    void *fiber[2];
    void *ctx[2];
    uint64_t ctx_size;
} Api;

/* Fiber A returns arg*10 three times, then switches to fiber B, which returns 7000+arg. */
static void entry_b(uint64_t arg_init, uint64_t arg_run) {
    Api *a = (Api *)arg_init;
    a->ret(7000 + arg_run, 0);
}
static void entry_a(uint64_t arg_init, uint64_t arg_run) {
    Api *a = (Api *)arg_init;
    uint64_t v = arg_run;
    volatile double keep = (double)(int64_t)arg_run;  /* an xmm value (1.0) that must survive the switches */
    for (int i = 0; i < 3; i++) {
        uint64_t next = 0;
        a->ret(v * 10, &next);
        v = next;
    }
    a->init(a->fiber[1], 0, entry_b, (uint64_t)a, a->ctx[1], a->ctx_size, 0, 0, 0);
    a->swtch(a->fiber[1], v + (uint64_t)(int64_t)(keep + keep), 0);
}

uint64_t fiber_test_main(Api *a) {
    a->init(a->fiber[0], 0, entry_a, (uint64_t)a, a->ctx[0], a->ctx_size, 0, 0, 0);
    uint64_t out = 0, sum = 0;
    for (uint64_t i = 1; i <= 4; i++) {
        a->run(a->fiber[0], i, &out);
        sum += out;
    }
    return sum; /* 10 + 20 + 30 + (7000 + 4 + 2) = 7066 */
}
