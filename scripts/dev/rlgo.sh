#!/bin/bash
# rlgo.sh: launch RL (interactive harness) and accept the EULA; drive the menus by hand afterwards.
: > /tmp/rl-ctl.txt; rm -rf /tmp/rlframes/*
(UL=${UL:-info} UTSURO_FAULTDUMP="$FD" nohup /tmp/rlint.sh >/dev/null 2>&1 &)
c() { echo "$*" >> /tmp/rl-ctl.txt; }
sleep 60; c "press A 200"; sleep 15
c "hold DOWN"; sleep 80; c "release DOWN"; sleep 3
c "press LEFT 300"; sleep 4; c "press A 200"
