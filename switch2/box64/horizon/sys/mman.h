/* <sys/mman.h> for the embedded Box64 on Horizon (newlib has none). Box64 maps only anonymous
 * memory for its own tables there (the host maps guest memory, the JIT has its own backend),
 * and in the embedded build it never write-protects pages. So mmap is an aligned allocation,
 * munmap frees a whole mapping and mprotect only reports success (os_embed_horizon.c). */
#ifndef GR_HORIZON_SYS_MMAN_H
#define GR_HORIZON_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void*)-1)

#define MADV_DONTNEED 4

#ifdef __cplusplus
extern "C" {
#endif
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t len, int prot);
int madvise(void* addr, size_t length, int advice);
#ifdef __cplusplus
}
#endif
#endif
