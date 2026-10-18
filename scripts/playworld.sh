#!/bin/bash
# playworld.sh -- starts a session and drives the menus into a new world (about 70 s).
cd "$(dirname "$0")/.."
scripts/session.sh start
sleep 22; scripts/ctl.sh press A 30        # Play Game
sleep 6;  scripts/ctl.sh press A 30        # Create New World
sleep 6;  scripts/ctl.sh press UP 20       # wraps to the last button
sleep 1.5; scripts/ctl.sh press A 30       # create it
sleep ${1:-35}
echo "in the world (probably)"
