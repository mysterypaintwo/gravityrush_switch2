/* getrandom for the runtime on Horizon (switch2/host/host_compat.h): the console's CSPRNG. */
#pragma once
#include <stddef.h>
#include <switch.h>
static inline long getrandom(void* buf, size_t len, unsigned flags)
{
    (void)flags;
    randomGet(buf, len);
    return (long)len;
}
