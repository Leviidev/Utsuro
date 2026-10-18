#!/bin/bash
# ctl.sh press A [ms] | stick LX 1.0 [ms] | hold A | release A | snap [file]   (talks to a session started with session.sh)
if [[ "$1" == "snap" ]]; then
    f=${2:-/tmp/snap.png}
    rm -f "$f"
    echo "snap $f" >> /tmp/utsuro-ctl.txt
    for i in $(seq 1 100); do [[ -s "$f" ]] && sleep 0.2 && break; sleep 0.1; done
    echo "$f"
else
    echo "$*" >> /tmp/utsuro-ctl.txt
fi
