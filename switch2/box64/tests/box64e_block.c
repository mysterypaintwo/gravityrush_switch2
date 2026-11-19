/* Runs one stretch of the game's own x86-64 code under the embedded Box64 and prints the
 * xmm registers it leaves, to compare the dynarec with the interpreter (BOX64_DYNAREC=0) on a
 * block Box64's test mode reported. The code is read from your eboot.elf at run time (nothing
 * of the game is in this file).
 *
 * Usage: box64e_block <eboot.elf> <start vaddr> <end vaddr>
 * The block runs with rbx = a zeroed 16 KiB buffer whose [rbx+0x218] points to a second one;
 * the test data below is what the player movement block at 0x380dbc-0x380e1c reads. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "box64e.h"

static uint8_t* read_file(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(*size);
    if (d && fread(d, 1, *size, f) != *size) { free(d); d = NULL; }
    fclose(f);
    return d;
}

/* The bytes at vaddr [start, end) of an ELF's PT_LOAD segments. */
static int elf_bytes(const uint8_t* d, size_t size, uint64_t start, uint64_t end, uint8_t* out)
{
    uint64_t phoff; uint16_t phnum;
    memcpy(&phoff, d + 32, 8); memcpy(&phnum, d + 56, 2);
    for (int i = 0; i < phnum; i++) {
        const uint8_t* ph = d + phoff + 56 * i;
        uint32_t type; uint64_t off, va, fsz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&va, ph + 16, 8); memcpy(&fsz, ph + 32, 8);
        if (type == 1 && start >= va && end <= va + fsz && off + (end - va) <= size) {
            memcpy(out, d + off + (start - va), end - start);
            return 0;
        }
    }
    return -1;
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <eboot.elf> <start> <end>\n", argv[0]); return 2; }
    const uint64_t start = strtoull(argv[2], NULL, 16), end = strtoull(argv[3], NULL, 16);
    size_t size;
    uint8_t* elf = read_file(argv[1], &size);
    if (!elf || end <= start || end - start > 4096) { fprintf(stderr, "bad input\n"); return 2; }

    box64e_jit jit;
    if (box64e_jit_create(16 << 20, &jit) || box64e_init(&jit)) { puts("box64e init failed"); return 1; }

    uint8_t* mem = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    size_t n = 0;
    static const uint8_t prologue[] = {
        0x53,                   /* push rbx */
        0x48, 0x89, 0xfb,       /* mov rbx, rdi */
        0xc5, 0xc8, 0x57, 0xf6, /* vxorps xmm6, xmm6, xmm6 */
        0xc5, 0xc0, 0x57, 0xff, /* vxorps xmm7, xmm7, xmm7 */
    };
    memcpy(mem + n, prologue, sizeof prologue); n += sizeof prologue;
    if (elf_bytes(elf, size, start, end, mem + n)) { fprintf(stderr, "range not in the ELF\n"); return 2; }
    n += end - start;
    for (int r = 0; r < 8; r++) { /* vmovdqu [rsi + 16*r], xmm<r> */
        const uint8_t st[] = {0xc5, 0xfa, 0x7f, (uint8_t)(0x46 | (r << 3)), (uint8_t)(r * 16)};
        memcpy(mem + n, st, sizeof st); n += sizeof st;
    }
    mem[n++] = 0x5b; /* pop rbx */
    mem[n++] = 0xc3; /* ret */
    box64e_set_protection((uintptr_t)mem, 1 << 16, PROT_READ | PROT_EXEC);

    uint8_t* a = calloc(1, 16384);
    uint8_t* c = calloc(1, 16384);
    float out[8][4];
    memcpy(a + 0x218, &c, sizeof c);
    const float va[4] = {0.6f, -0.8f, 0.25f, 0.0f}, vc[4] = {0.5f, 0.7f, 0.2f, 0.0f};
    memcpy(a + 0x33d0, va, sizeof va);
    memcpy(c + 0x650, vc, sizeof vc);
    for (int run = 0; run < 3; run++) /* the first run may be interpreted, the next translated */
        box64e_call_guest((uintptr_t)mem, 2, (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)out);
    for (int r = 0; r < 8; r++)
        printf("xmm%d: %g %g %g %g\n", r, out[r][0], out[r][1], out[r][2], out[r][3]);
    return 0;
}
