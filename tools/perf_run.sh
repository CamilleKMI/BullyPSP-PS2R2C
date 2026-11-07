#!/bin/bash
# perf_run.sh TAG SECONDS [harness args] - runs the PC build compiled like the PSP one (-O2, no access checks:
# /root/ps2rc-build/perfhost) with no graphics (HLE_VU1=0), 32 MB - 64 KB like the PSP, and prints its PERF lines
# (cpu ms per game frame) from game frame 2000 on. The check build is put back afterwards.
TAG=$1; SECS=$2; shift 2
B=/root/ps2rc-build/SLES-53561_v1.01_host
R=$(cd "$(dirname "$0")/.." && pwd)
cp "$B/ps2rc_host" "$B/ps2rc_host.check"
cp /root/ps2rc-build/perfhost/ps2rc_host "$B/ps2rc_host"
HLE_VU1=0 HLE_SAMPLE="$SAMPLE" HLE_SAMPLE_FROM=40 "$R/tools/run_guarded.sh" "$TAG" "$SECS" --ram-end 1ff0000 "$@" > /dev/null 2>&1
cp "$B/ps2rc_host.check" "$B/ps2rc_host"
grep "^PERF" "$R/work/runs/host_$TAG.log" | awk '{split($2,a,":"); if (a[1]+0 >= 2000) print}' | cut -c1-170
