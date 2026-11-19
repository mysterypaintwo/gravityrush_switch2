/* <dlfcn.h> for the embedded Box64 on Horizon: no dynamic libraries, every lookup fails. */
#ifndef GR_HORIZON_DLFCN_H
#define GR_HORIZON_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT ((void*)-1)
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
#ifdef __cplusplus
extern "C" {
#endif
void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
int dladdr(const void* addr, Dl_info* info);
#ifdef __cplusplus
}
#endif
#endif
