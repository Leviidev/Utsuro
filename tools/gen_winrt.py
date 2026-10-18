"""Generates src/nt/winrt_generated.inc from WinDurango's WinRT IDL notes.

usage: gen_winrt.py <idl dir> <out.inc>

For each runtimeclass the IDL gives an interface (IID from interface_name), optionally a
statics interface (static_name), and members in vtable order. Each member becomes a method
whose generic default is derived from its type; code in winrt_classes.cpp overrides the
ones that matter.
"""
import re, sys, glob, os

idl_dir, out_path = sys.argv[1], sys.argv[2]

SIZES = {  # IDL type -> Out kind
    'Boolean': 'Bool', 'Int8': 'Bool', 'UInt8': 'Bool', 'Char16': 'I32', 'Int16': 'I32', 'UInt16': 'I32',
    'Int32': 'I32', 'UInt32': 'I32', 'Single': 'I32', 'HRESULT': 'I32', 'unsigned int': 'I32', 'int': 'I32',
    'Int64': 'I64', 'UInt64': 'I64', 'Double': 'I64', 'DateTime': 'I64', 'TimeSpan': 'I64',
    'Guid': 'GuidT', 'String': 'Ptr', 'Object': 'Ptr', 'void': 'None',
}

text = ''
guessed = set()   # GUIDs the IDL's comments call placeholders or unknown
for f in sorted(glob.glob(os.path.join(idl_dir, '*.idl'))):
    raw = open(f).read()
    lines = raw.split('\n')
    for i, line in enumerate(lines):
        m = re.search(r'(?:interface_name|static_name)\(\s*"\w+"\s*,\s*([0-9A-Fa-f-]{36})', line)
        if m:
            context = '\n'.join(lines[max(0, i - 3):i + 1]).lower()
            if 'placeholder' in context or 'is unknown' in context or 'todo' in context:
                guessed.add(m.group(1).lower())
    text += raw + '\n'
text = re.sub(r'//[^\n]*', '', text)
text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)

enums = set(re.findall(r'\benum\s+(\w+)', text))
# struct sizes (rough): sum of field sizes, 8-aligned
struct_sizes = {}
for m in re.finditer(r'\bstruct\s+(\w+)\s*\{(.*?)\}', text, flags=re.S):
    total = 0
    for fld in re.findall(r'([\w\.<>]+)\s+\w+\s*;', m.group(2)):
        s = SIZES.get(fld.split('.')[-1], 'Ptr')
        total += {'Bool': 1, 'I32': 4, 'I64': 8, 'GuidT': 16, 'Ptr': 8, 'None': 0}[s]
    struct_sizes[m.group(1)] = total

def out_kind(t):
    t = t.strip()
    base = t.split('.')[-1]
    if base in SIZES: return SIZES[base]
    if base in enums: return 'I32'
    if base in struct_sizes:
        return 'Struct%d' % struct_sizes[base]
    return 'Ptr'

# split off namespaces and classes
interfaces = {}   # name -> (uuid, body)
for m in re.finditer(r'((?:\[[^\]]*\]\s*)*)interface\s+(\w+)\s*(?:requires\s+[\w\.,\s]+)?\{', text):
    attrs, name = m.group(1), m.group(2)
    u = re.search(r'uuid\(\s*([0-9A-Fa-f-]{36})', attrs)
    depth, i = 1, m.end()
    while depth and i < len(text):
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    interfaces[name] = (u.group(1) if u else None, text[m.end():i - 1])
classes = []
ns_stack = []
pos = 0
tokens = re.finditer(r'namespace\s+([\w\.]+)\s*\{|\}|((?:\[[^\]]*\]\s*)*)runtimeclass\s+(\w+)\s*(?:requires\s+[\w\.,\s]+)?\{(.*?)\n\s*\}', text, flags=re.S)
# A simpler, robust approach: find each runtimeclass block with its preceding attributes and namespace by scanning.
ns_ranges = []   # (start, end, fullname) for every namespace block, nested names joined
def scan_namespaces(span_start, span_end, prefix):
    for m in re.finditer(r'namespace\s+([\w\.]+)\s*\{', text[span_start:span_end]):
        start = span_start + m.start()
        depth, i = 1, span_start + m.end()
        while depth and i < span_end:
            if text[i] == '{': depth += 1
            elif text[i] == '}': depth -= 1
            i += 1
        full = (prefix + '.' if prefix else '') + m.group(1)
        ns_ranges.append((start, i, full))
# Only top-level blocks need scanning; nested ones are found by recursion.
def scan_all(span_start, span_end, prefix):
    pos = span_start
    while True:
        m = re.search(r'namespace\s+([\w\.]+)\s*\{', text[pos:span_end])
        if not m: break
        start = pos + m.start()
        depth, i = 1, pos + m.end()
        while depth and i < span_end:
            if text[i] == '{': depth += 1
            elif text[i] == '}': depth -= 1
            i += 1
        full = (prefix + '.' if prefix else '') + m.group(1)
        ns_ranges.append((start, i, full))
        scan_all(pos + m.end(), i - 1, full)
        pos = i
scan_all(0, len(text), '')
def namespace_at(p):
    best = ''
    for s, e, n in ns_ranges:
        if s <= p < e and len(n) > len(best): best = n
    return best

for m in re.finditer(r'((?:\[[^\]]*\]\s*)*)runtimeclass\s+(\w+)\s*(?::[^{]*|requires[^{]*)?\{', text):
    attrs, name = m.group(1), m.group(2)
    header = text[m.start(2):m.end()]
    default_if = re.search(r':\s*\[default\]\s*(\w+)', header)
    # find matching close brace
    depth, i = 1, m.end()
    while depth and i < len(text):
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    body = text[m.end():i - 1]
    iface = re.search(r'interface_name\(\s*"(\w+)"\s*,\s*([0-9A-Fa-f-]{36})', attrs)
    stat = re.search(r'static_name\(\s*"(\w+)"\s*,\s*([0-9A-Fa-f-]{36})', attrs)
    iface_t = iface.groups() if iface else None
    if iface_t is None and default_if and default_if.group(1) in interfaces and interfaces[default_if.group(1)][0]:
        d = interfaces[default_if.group(1)]
        iface_t = (default_if.group(1), d[0])
        body = d[1] + '\n' + body
    classes.append((namespace_at(m.start()), name, iface_t, stat.groups() if stat else None, body))

def split_members(body):
    members, cur, depth = [], '', 0
    for ch in body:
        cur += ch
        if ch in '<({': depth += 1
        elif ch in '>)}': depth -= 1
        elif ch == ';' and depth == 0:
            members.append(cur.strip()); cur = ''
    return members

def count_params(argtext):
    argtext = argtext.strip()
    if not argtext: return 0
    n, depth, any_ = 1, 0, False
    for ch in argtext:
        if ch in '<([': depth += 1
        elif ch in '>)]': depth -= 1
        elif ch == ',' and depth == 0: n += 1
    return n

out = []
emit = out.append
emit('// GENERATED by tools/gen_winrt.py from WinDurango\'s IDL notes. Do not edit.')
emit('static void RegisterGenerated(Runtime& rt) {')
count_ifaces = 0
for ns, name, iface, stat, body in classes:
    inst, statics = [], []
    has_ctor = False
    for mem in split_members(body):
        mem = re.sub(r'\[[^\]]*\]', '', mem).strip().rstrip(';').strip()
        if not mem: continue
        is_static = bool(re.match(r'static\s+', mem))
        mem = re.sub(r'^static\s+', '', mem)
        target = statics if is_static else inst
        if re.match(r'%s\s*\(' % re.escape(name), mem):
            has_ctor = has_ctor or mem.replace(' ', '') == name + '()'
            continue
        ev = re.match(r'event\s+(.+?)\s+(\w+)$', mem, flags=re.S)
        if ev:
            target.append(('add_' + ev.group(2), 'Add')); target.append(('remove_' + ev.group(2), 'Remove')); continue
        prop = re.match(r'(.+?)\s+(\w+)\s*\{\s*(get;)?\s*(set;)?\s*\}$', mem, flags=re.S)
        if prop:
            t, pname, g, s = prop.groups()
            if g: target.append(('get_' + pname, 'Get:' + out_kind(t)))
            if s: target.append(('put_' + pname, 'Put'))
            continue
        meth = re.match(r'(.+?)\s+(\w+)\s*\((.*)\)$', mem, flags=re.S)
        if meth:
            t, mname, args = meth.groups()
            n = count_params(args)
            k = out_kind(t)
            # A method's result comes back through a trailing out parameter.
            target.append((mname, 'Method:%s:%d' % (k, n)))
    full = ns + '.' + name
    def emit_iface(var, iname, guid, methods):
        emit('    Iface& %s = rt.DefineInterface("%s", "%s", {' % (var, iname, guid))
        if guid.lower() in guessed: emit('    // guessed GUID')
        for mname, kind in methods:
            if kind == 'Add': emit('        Add("%s"),' % mname)
            elif kind == 'Remove': emit('        Remove("%s"),' % mname)
            elif kind == 'Put': emit('        Put("%s"),' % mname)
            elif kind.startswith('Get:'): emit('        Get("%s", %s),' % (mname, kind[4:] if not kind[4:].startswith('Struct') else 'Out::Ptr'))
            else:
                _, k, n = kind.split(':')
                emit('        Result("%s", %s, %s),' % (mname, k if not k.startswith('Struct') else 'Ptr', n))
        emit('    }, %s);' % ('true' if guid.lower() in guessed else 'false'))
    var_base = full.replace('.', '_')
    iv = sv = None
    emit('    {')
    if iface and inst:
        iv = 'i_' + var_base
        emit_iface(iv, iface[0], iface[1], inst)
    if stat and statics:
        sv = 's_' + var_base
        emit_iface(sv, stat[0], stat[1], statics)
    if iv or sv or has_ctor:
        creator = ('[&](Runtime& r) -> Obj* { return r.NewObject("%s", {&%s}); }' % (full, iv)) if iv else '[](Runtime&) -> Obj* { return nullptr; }'
        emit('        rt.DefineClass("%s", %s, %s);' % (full, creator, ('&' + sv) if sv else 'nullptr'))
    emit('    }')
emit('}')
open(out_path, 'w').write('\n'.join(out) + '\n')
print(len(classes), 'classes')
