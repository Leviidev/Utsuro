#!/Users/davi/Documents/Coding/Utsuro/tools/venv/bin/python
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

os.system('pkill -f "[u]tsuro-run"; mkdir -p /tmp/rlframes; find /tmp/rlframes -name "*.png" -delete; : > /tmp/rl-ctl.txt')
subprocess.Popen([os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rlint.sh')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
t0 = time.time()
def log(*a): print(f'[{time.time()-t0:5.0f}s f{frame_no()}]', *a, flush=True)

while frame_no() < 1500: time.sleep(3)
log('start screen'); press('A', 200, 3)
time.sleep(25)
if os.environ.get('EULA', '0') == '1':   # only on a fresh save: once accepted, the save remembers it and the menu comes straight up
    log('eula (assumed)')
    base = accept_crop()
    cmd('hold DOWN'); th = time.time()
    # the Accept label changes once the text has been read to the end (not before a minute: the dialog fades in)
    while time.time() - th < 60 or sum(ImageStat.Stat(ImageChops.difference(base, accept_crop())).mean) < 15: time.sleep(2)
    cmd('release DOWN'); time.sleep(2); log('accept enabled')
    press('LEFT', 150, 3); press('A', 150, 3)
else:
    log('no eula (save remembers it)')
time.sleep(20); wait_frames(3)
press('B', 120, 3); press('B', 120, 4)   # the menu comes up on Options with its dropdown open
for _ in range(7): press('UP', 120, 1.5)
press('A', 120, 6); press('DOWN', 120, 4)
press('A', 120, 6); press('A', 120, 8)
if os.environ.get('NAV_ONLY'): log('on the exhibition screen'); sys.exit(0)
# The Exhibition screen remembers its cursor row and game mode between runs, and Game Mode wraps (Soccar <- Dropshot), so go
# to the top row and step LEFT until the mode reads SOCCAR (a crop saved from a run, build/ref/rl_soccar.png). Then the Arena
# row: RANDOM can land on a Rocket Labs map (dark neon by design), so ARENA_STEPS (default 1, DFH Stadium) presses of RIGHT.
# Arena is row 1 of 7; Create Match is 5 rows below it.
for _ in range(8): press('UP', 120, 1.2)
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../build/ref/rl_soccar.png')
def mode_crop(): return img().crop((330, 520, 640, 550))
if os.path.exists(REF):
    ref = Image.open(REF).convert('RGB')
    for _ in range(6):
        diff = sum(ImageStat.Stat(ImageChops.difference(ref, mode_crop())).mean) / 3
        if diff < 12: break
        press('LEFT', 120, 2.5)
    log(f'game mode diff from SOCCAR {diff:.1f}')
else:
    log('no SOCCAR reference crop: leaving the game mode as it is')
press('DOWN', 120, 1.5)
for _ in range(int(os.environ.get('ARENA_STEPS', '1'))): press('RIGHT', 120, 1.5)
img().save('/tmp/rlframes/arena_choice.png')
for _ in range(5): press('DOWN', 120, 1.2)
log('create match'); press('A', 120, 3)
log('done; frames keep dumping')
time.sleep(85); press('A', 120, 3); log('team chosen')
if os.environ.get('NO_TRACE'):
    time.sleep(10)   # past the kickoff countdown
    rates = []
    for _ in range(3):
        t1, f1 = time.time(), frame_no(); time.sleep(10); rates.append((frame_no() - f1) / (time.time() - t1))
    log('fps over three 10 s windows in the match: ' + ' '.join(f'{r:.1f}' for r in rates))
else:
    time.sleep(30); cmd('trace'); log('traced')
if os.environ.get('SCORE'):
    # A straight drive from the kickoff spot scores; then the goal replay and the kickoff reset (a different camera).
    cmd('hold RT'); time.sleep(12); cmd('release RT'); log('scored?')
    time.sleep(25); img().save('/tmp/rlframes/after_goal.png'); log('after goal')
