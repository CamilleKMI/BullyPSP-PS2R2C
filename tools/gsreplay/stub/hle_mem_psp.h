/* hle_mem_psp.h stand-in for the replayer: no volatile partition */
#include <stddef.h>
static inline void *hle_mem_vol_take(size_t size, size_t keep) { (void)size; (void)keep; return 0; }
