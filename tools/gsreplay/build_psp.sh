#!/bin/bash
# build_psp.sh - builds the PSP graphics replayer into work/gsreplay/psp (needs pspdev on PATH)
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd); P=$R/packs/SLES-53561_v1.01/src
B=$R/work/gsreplay/psp; mkdir -p "$B"
cp "$H/main_psp.c" "$H/gsreplay_core.c" "$H/stub/hle.h" "$H/stub/hle_mem_psp.h" "$H/stub/hle_plat.h" "$H/stub/hle_cfg.h" "$P/hle_gfx_scan.c" "$P/hle_vu1.c" "$P/hle_vu1_native.h" "$P/gs_mem.h" "$P/psp/hle_ge_gs.c" "$B/"
[ -f "$R/work/vu1aot/hle_vu1_aot.h" ] && cp "$R/work/vu1aot/hle_vu1_aot.h" "$B/"
[ -f "$R/work/vu1aot/hle_vu1_fast.h" ] && cp "$R/work/vu1aot/hle_vu1_fast.h" "$B/"
cp "$H/Makefile.psp" "$B/Makefile"
cd "$B" && make -s
