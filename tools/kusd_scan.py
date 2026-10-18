import sys, pefile, re
from collections import Counter
for path in sys.argv[1:]:
    pe = pefile.PE(path, fast_load=True)
    data = pe.get_memory_mapped_image()
    hits = [m.start() for m in re.finditer(rb'[\x00-\xff]\x0f\xfe\x7f|\x00\x00\xfe\x7f', data)]
    pat = [m.start() for m in re.finditer(rb'\x00[\x00-\x0f]\xfe\x7f', data)]
    ctx = Counter()
    for p in pat:
        ctx[bytes(data[p-3:p]).hex()] += 1
    print(path.split('/')[-1], len(pat), ctx.most_common(8))
