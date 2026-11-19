/* The game's memory without a system resource (Switch 2).
 *
 * The runtime maps the guest's memory where the game expects it with svcMapPhysicalMemory
 * (src/runtime_memory.c: the pool on demand, the low maps). That call needs the process's system
 * resource, which comes from the program the homebrew menu runs as: some ways into HEN run it as
 * one with none, and every map fails with 0xfa01 (a tester's console, 2026-11-19: program
 * 040074a01bf12000, 0 KiB). Mirroring heap memory into the alias region with svcMapMemory does
 * not work either: the kernel only takes its destination in the stack region (0xdc01).
 *
 * Without one, the guest's memory is the heap's top itself: gr_switch.c leaves it in place
 * (what it would otherwise give back) and lays the guest out inside it (low memory, then the
 * pool at its physical offsets). It is already mapped, so a "map" only checks that the range is
 * inside and zeroes it (it was the loader's), and an unmap does nothing: the heap goes back to
 * its first size at exit. */
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool fallback;
static uintptr_t pool_start, pool_end;

/* gr_switch.c, before any map: [start, end) of the heap is the guest's. */
void gr_physmem_use_heap(uintptr_t start, uintptr_t end)
{
    fallback = true;
    pool_start = (start + 0x3fff) & ~(uintptr_t)0x3fff;
    pool_end = end & ~(uintptr_t)0x3fff;
}

bool gr_physmem_fallback(void) { return fallback; }

/* The guest's part of the heap (gr_switch.c lays the guest out in it). */
void gr_physmem_heap_range(uintptr_t* start, uintptr_t* end)
{
    *start = pool_start;
    *end = pool_end;
}

Result gr_map_physical(void* address, size_t size)
{
    if (!fallback) return svcMapPhysicalMemory(address, size);
    const uintptr_t at = (uintptr_t)address;
    if (at < pool_start || at + size > pool_end || at + size < at)
        return MAKERESULT(Module_Kernel, KernelError_OutOfMemory);
    memset(address, 0, size);
    return 0;
}

Result gr_unmap_physical(void* address, size_t size)
{
    if (!fallback) return svcUnmapPhysicalMemory(address, size);
    (void)address;
    (void)size;
    return 0;
}
