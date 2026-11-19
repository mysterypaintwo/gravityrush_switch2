/* Runs every SSE/AVX instruction form the game uses (simd_forms.py writes them from your
 * eboot.elf) once each under the embedded Box64, with the same pseudo-random registers and
 * memory, and writes the resulting xmm/ymm-low, general registers and flags. Run it twice, with
 * the dynarec and with BOX64_DYNAREC=0 (the interpreter), and compare with simd_compare.py: a
 * form whose results differ is translated wrongly by one of them.
 *
 * Usage: box64e_simdcheck <forms.bin> <results.bin>
 * Each form runs as its own small function: load xmm0-15, point every general register except
 * rsp at the middle of an 8 MiB buffer of pseudo-random data, the instruction, store the
 * results. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "box64e.h"

#define BUF_SIZE (8u << 20)
#define CODE_SIZE 4096

typedef struct {
    uint8_t xmm[16][16];
    uint64_t gpr[16];
    uint64_t flags;
} Result;

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t next_u32(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}
/* Mostly ordinary floats (what game code computes with), some integers and special values. */
static uint32_t lane_value(void)
{
    const uint32_t r = next_u32();
    switch (r % 8) {
    case 0: return next_u32();                                   /* any bits */
    case 1: return (uint32_t)(int32_t)((int)(next_u32() % 2001) - 1000); /* small integer */
    case 2: { const float f = 0.0f; uint32_t u; memcpy(&u, &f, 4); return u ^ ((r >> 8) & 1) << 31; }
    default: {
        float f = ((float)(next_u32() % 2000001) - 1000000.0f) / 250000.0f; /* -4..4 */
        uint32_t u; memcpy(&u, &f, 4); return u;
    }
    }
}

static size_t emit(uint8_t* p, size_t n, const uint8_t* b, size_t len) { memcpy(p + n, b, len); return n + len; }
static size_t emit_u32(uint8_t* p, size_t n, uint32_t v) { memcpy(p + n, &v, 4); return n + 4; }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <forms.bin> <results.bin>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    uint32_t count;
    if (fread(&count, 4, 1, f) != 1) return 2;

    box64e_jit jit;
    if (box64e_jit_create(64 << 20, &jit) || box64e_init(&jit)) { puts("box64e init failed"); return 1; }

    uint8_t* buf = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t* pristine = malloc(BUF_SIZE);
    for (size_t i = 0; i < BUF_SIZE; i += 4) { const uint32_t v = lane_value(); memcpy(pristine + i, &v, 4); }
    uint8_t (*xmm_in)[16][16] = malloc(sizeof *xmm_in * count);
    for (uint32_t k = 0; k < count; k++)
        for (int r = 0; r < 16; r++)
            for (int l = 0; l < 4; l++) { const uint32_t v = lane_value(); memcpy(&xmm_in[k][r][l * 4], &v, 4); }
    Result* out = mmap(NULL, sizeof(Result), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t* code = mmap(NULL, CODE_SIZE * (size_t)count, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    FILE* res = fopen(argv[2], "wb");
    if (!buf || !pristine || !out || !code || !res) { puts("out of memory"); return 1; }
    const uint64_t center = (uint64_t)(uintptr_t)(buf + BUF_SIZE / 2);

    for (uint32_t k = 0; k < count; k++) {
        uint8_t len, ins[16];
        if (fread(&len, 1, 1, f) != 1 || len > 15 || fread(ins, 1, len, f) != len) return 2;
        uint8_t* p = code + (size_t)k * CODE_SIZE;
        size_t n = 0;
        static const uint8_t save[] = {0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
        n = emit(p, n, save, sizeof save); /* push rbx, rbp, r12-r15 */
        for (int r = 0; r < 16; r++) {     /* vmovdqu xmm<r>, [rdi + 16 r] */
            const uint8_t ld[] = {0xc4, (uint8_t)(r < 8 ? 0xe1 : 0x61), 0x7a, 0x6f, (uint8_t)(0x87 | ((r & 7) << 3))};
            n = emit(p, n, ld, sizeof ld);
            n = emit_u32(p, n, (uint32_t)(16 * r));
        }
        for (int t = 0; t < 16; t++) {     /* mov <t>, rsi (the buffer's middle), all but rsp */
            if (t == 4 || t == 6) continue;
            const uint8_t mv[] = {(uint8_t)(0x48 | (t >= 8)), 0x89, (uint8_t)(0xc0 | (6 << 3) | (t & 7))};
            n = emit(p, n, mv, sizeof mv);
        }
        n = emit(p, n, ins, len);
        const uint8_t push_r11[] = {0x41, 0x53};
        n = emit(p, n, push_r11, sizeof push_r11);
        const uint8_t movabs_r11[] = {0x49, 0xbb};
        n = emit(p, n, movabs_r11, sizeof movabs_r11);
        const uint64_t outp = (uint64_t)(uintptr_t)out;
        memcpy(p + n, &outp, 8); n += 8;
        for (int r = 0; r < 16; r++) {     /* vmovdqu [r11 + 16 r], xmm<r> */
            const uint8_t st[] = {0xc4, (uint8_t)(r < 8 ? 0xc1 : 0x41), 0x7a, 0x7f, (uint8_t)(0x83 | ((r & 7) << 3))};
            n = emit(p, n, st, sizeof st);
            n = emit_u32(p, n, (uint32_t)(16 * r));
        }
        for (int t = 0; t < 16; t++) {     /* mov [r11 + 256 + 8 t], <t> (not rsp, r11) */
            if (t == 4 || t == 11) continue;
            const uint8_t st[] = {(uint8_t)(0x49 | ((t >= 8) << 2)), 0x89, (uint8_t)(0x83 | ((t & 7) << 3))};
            n = emit(p, n, st, sizeof st);
            n = emit_u32(p, n, (uint32_t)(256 + 8 * t));
        }
        /* flags: pushfq; pop rax; and with the status flags; mov [r11 + 384], rax */
        static const uint8_t fl[] = {0x9c, 0x58, 0x48, 0x25, 0xd5, 0x08, 0x00, 0x00, 0x49, 0x89, 0x83, 0x80, 0x01, 0x00, 0x00};
        n = emit(p, n, fl, sizeof fl);
        static const uint8_t restore[] = {0x41, 0x5b, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3};
        n = emit(p, n, restore, sizeof restore); /* pop r11, r15-r12, rbp, rbx; ret */
        if (n > CODE_SIZE) return 3;
    }
    fclose(f);
    box64e_set_protection((uintptr_t)code, CODE_SIZE * (size_t)count, PROT_READ | PROT_EXEC);

    const int verbose = getenv("SIMDCHECK_VERBOSE") != NULL;
    for (uint32_t k = 0; k < count; k++) {
        if (verbose) { fprintf(stderr, "form %u\n", k); fflush(stderr); }
        memcpy(buf, pristine, BUF_SIZE);
        memset(out, 0, sizeof *out);
        box64e_call_guest((uintptr_t)(code + (size_t)k * CODE_SIZE), 2,
                          (uint64_t)(uintptr_t)xmm_in[k], center);
        /* Store results relative to the buffer, so both runs compare equal for pointers. */
        for (int t = 0; t < 16; t++)
            if (out->gpr[t] >= (uint64_t)(uintptr_t)buf && out->gpr[t] < (uint64_t)(uintptr_t)buf + BUF_SIZE)
                out->gpr[t] -= (uint64_t)(uintptr_t)buf;
        fwrite(out, sizeof *out, 1, res);
        /* A hash of the buffer catches instructions that store to memory. */
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < BUF_SIZE; i += 8) { uint64_t v; memcpy(&v, buf + i, 8); h = (h ^ v) * 1099511628211ull; }
        fwrite(&h, 8, 1, res);
    }
    fclose(res);
    printf("%u forms run\n", count);
    return 0;
}
