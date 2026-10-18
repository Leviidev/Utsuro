#!/bin/bash
# run.sh [seconds] [tail lines] -- runs the title for a while (killing it for sure afterwards), keeps
# the log in /tmp/run.log and prints what happened after the view started.
cd "$(dirname "$0")/.."
T=${1:-20}
UTSURO_LOG=${UTSURO_LOG:-info} ./build/utsuro-run ${UTSURO_ARGS:-games/mc-1.2.0.0} > /tmp/run.log 2>&1 &
PID=$!
for ((i = 0; i < T * 5; i++)); do kill -0 $PID 2>/dev/null || break; sleep 0.2; done
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
grep -v "^\[ut:D\]" /tmp/run.log | grep -v "^\[ut:W\]     \[rsp" | sed -n '/Load done/,$p' | cut -c1-200 | tail -${2:-45}
echo "($(wc -l < /tmp/run.log) log lines)"
