#!/bin/bash
# snap.sh [n]: montage of the last n dumped frames -> /tmp/rl-t.png
/tmp/pyv/bin/python - "${1:-1}" <<'P'
import sys, glob
from PIL import Image
n=int(sys.argv[1]); fs=sorted(glob.glob('/tmp/rlframes/frame_*.png'))[-n:]
m=Image.new('RGB',(960*n,540))
for i,f in enumerate(fs): m.paste(Image.open(f).convert('RGB').resize((960,540)),(i*960,0))
m.save('/tmp/rl-t.png'); print(fs[-1])
P
