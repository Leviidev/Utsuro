#!/bin/bash
cd ~/Documents/Coding/Utsuro
env ${RL_ENV:-} UTSURO_TRACEFRAME=999999999 UTSURO_CTL=/tmp/rl-ctl.txt UTSURO_DUMPFRAMES=/tmp/rlframes UTSURO_DUMPEVERY=100 UTSURO_HEADLESS=1 UTSURO_LOG=${UL:-info} ./build/utsuro-run "/Volumes/macOS Storage/utsuro-games/rocket-league-1.0.4.2" TAGame-Dingo-Shipping.exe > /tmp/rl-int.log 2>&1
echo "exit status $? at $(date +%T)" > /tmp/rl-status.txt
