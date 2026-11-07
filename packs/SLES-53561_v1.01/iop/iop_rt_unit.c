/* iop_rt_unit.c - PS2RC's generic IOP runtime (ps2rc/iop/rt, staged as headers by convert/iopgen.py) as one unit,
   kept apart from hle.h (its internal names overlap the core's). */
#ifdef HLE_IOP_SND
#include "iop_rt_impl.h"
#include "iop_rt_libs_impl.h"
#ifndef HLE_PSP
#include "iop_rt_ucontext_impl.h"
#endif
#endif
