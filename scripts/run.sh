#!/bin/bash
# run.sh [seconds] -- runs the title, keeps the log in /tmp/run.log and prints what happened after the
# last "first call" burst: warnings, the fault, and the unknown interfaces.
cd "$(dirname "$0")/.."
UTSURO_LOG=${UTSURO_LOG:-info} perl -e 'alarm shift; exec @ARGV' "${1:-20}" ./build/utsuro-run games/mc-1.2.0.0 > /tmp/run.log 2>&1
grep -v "^\[ut:D\]" /tmp/run.log | grep -v "^\[ut:W\]     \[rsp" | sed -n '/Load done/,$p' | cut -c1-200 | tail -${2:-45}
echo "($(wc -l < /tmp/run.log) log lines)"
