#!/tmp/pyv/bin/python
"""rlmatch.py: launch RL on the Mac harness and drive it from the title to an exhibition match, reading the dumped frames."""
import glob, os, subprocess, sys, time
from PIL import Image, ImageStat, ImageChops

CTL = '/tmp/rl-ctl.txt'
FR = '/tmp/rlframes'
def cmd(c): open(CTL, 'a').write(c + '\n')
def latest():
    fs = sorted(glob.glob(FR + '/frame_*.png'))
    return fs[-1] if fs else None
def frame_no():
    f = latest(); return int(f[-10:-4]) if f else 0
def img():
    for _ in range(5):
        try:
            return Image.open(latest()).convert('RGB')
        except Exception: time.sleep(0.5)
    return Image.new('RGB', (1920, 1080), (0, 0, 0))
def bright(box): return sum(ImageStat.Stat(img().crop(box)).mean) / 3
def press(n, ms=120, wait=2.5): cmd(f'press {n} {ms}'); time.sleep(wait)
def wait_frames(n):
    f = frame_no()
    while frame_no() < f + n: time.sleep(1)
def menu_row():
    # main menu rows (full res) at y=606+42k; the selected one is a light bar
    best, idx = -1, None
    for k in range(6):
        box = (60, 596 + 42 * k, 330, 616 + 42 * k)
        n = sum(1 for p in img().crop(box).get_flattened_data() if min(p) > 170)
        if n > best: best, idx = n, k
    return idx, best
def accept_crop(): return img().crop((760, 830, 950, 875))

os.system('pkill -f "[u]tsuro-run"; rm -rf /tmp/rlframes/*; : > /tmp/rl-ctl.txt')
subprocess.Popen(['/tmp/rlint.sh'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
t0 = time.time()
def log(*a): print(f'[{time.time()-t0:5.0f}s f{frame_no()}]', *a, flush=True)

while frame_no() < 1500: time.sleep(3)
log('start screen'); press('A', 200, 3)
time.sleep(25); log('eula (assumed)')
base = accept_crop()
cmd('hold DOWN'); th = time.time()
# the Accept label changes once the text has been read to the end (not before a minute: the dialog fades in)
while time.time() - th < 60 or sum(ImageStat.Stat(ImageChops.difference(base, accept_crop())).mean) < 15: time.sleep(2)
cmd('release DOWN'); time.sleep(2); log('accept enabled')
press('LEFT', 150, 3); press('A', 150, 3)
time.sleep(20); wait_frames(3)
press('B', 120, 3); press('B', 120, 4)   # the menu comes up on Options with its dropdown open
for _ in range(7): press('UP', 120, 1.5)
press('A', 120, 6); press('DOWN', 120, 4)
press('A', 120, 6); press('A', 120, 8)
log('create match'); press('A', 120, 3)
log('done; frames keep dumping')
time.sleep(85); press('A', 120, 3); log('team chosen')
time.sleep(30); cmd('trace'); log('traced')
