#!/bin/bash
# session.sh start|stop -- a long-lived headless run you can drive with scripts/ctl.sh.
cd "$(dirname "$0")/.."
case "$1" in
start)
    pkill -9 -f "build/utsuro-run" 2>/dev/null; sleep 0.5
    : > /tmp/utsuro-ctl.txt
    UTSURO_HEADLESS=1 UTSURO_CTL=/tmp/utsuro-ctl.txt UTSURO_LOG=${UTSURO_LOG:-info} nohup ./build/utsuro-run ${UTSURO_ARGS:-games/mc-1.2.0.0} > /tmp/run.log 2>&1 &
    echo "started $!"
    ;;
stop)
    pkill -9 -f "build/utsuro-run"
    ;;
esac
