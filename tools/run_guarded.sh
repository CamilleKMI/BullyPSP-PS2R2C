#!/bin/bash
# run_guarded.sh TAG SECONDS [extra harness args] - mk/run_host.sh for the pack with a 300 MB cap on the run log
# (a module that logs in a loop once wrote 29 GB) and the empty memory card.
TAG=$1; SECS=$2; shift 2
B=/root/ps2rc-build/SLES-53561_v1.01_host
cd "$(dirname "$0")/../PS2RC" || exit 1
export PS2RC_LOCAL=$(cd .. && pwd)/local.toml
bash mk/run_host.sh --pack SLES-53561_v1.01 "$SECS" 0 "$TAG" --memcard "$(cd .. && pwd)/work/memcard" "$@" > /dev/null 2>&1 &
P=$!
while kill -0 $P 2>/dev/null; do
  s=$(stat -c %s "$B/run_$TAG.log" 2>/dev/null || echo 0)
  if [ "$s" -gt 300000000 ]; then pkill -f "ps2rc_host.*run_$TAG.log"; echo "GUARD: log over 300 MB, run stopped"; fi
  timeout 2 tail -f /dev/null
done
rm -f "$B/run_$TAG.log"
echo "run $TAG done"
