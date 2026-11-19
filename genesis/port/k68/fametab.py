# fametab.py: parses PicoDrive's FAME (famec.c init_jump_table + famec_opcodes.h)
# -> op2h[65536] handler names, and per handler the RET(...) cycle expressions.
import re, os
P = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'picodrive', 'cpu', 'fame')

def _cpp(lines, defs):
    out, stack = [], []
    for l in lines:
        s = l.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|else|endif|elif)\b\s*(.*)', s)
        if m:
            k, a = m.group(1), m.group(2).strip()
            if k == 'ifdef': stack.append(a in defs)
            elif k == 'ifndef': stack.append(a not in defs)
            elif k == 'if': stack.append(a.strip() not in ('0',))
            elif k == 'else': stack[-1] = not stack[-1]
            elif k == 'endif': stack.pop()
            continue
        if all(stack): out.append(l)
    return out

DEFS = {'USE_CYCLONE_TIMING', 'USE_CYCLONE_TIMING_DIV', 'PICODRIVE_HACK', 'FAMEC_CHECK_BRANCHES',
        'FAMEC_EMULATE_TRACE', 'FAMEC_ROLL_INLINE', 'FAMEC_NO_GOTOS', 'FAMEC_32BIT_PC', 'FAMEC_EXTRA_INLINE'}

def jumptable():
    src = open(os.path.join(P, 'famec.c')).read().split('\n')
    a = next(i for i, l in enumerate(src) if 'JumpTable[0x0000 + i] = CAST_OP(0x4AFC)' in l) - 1
    b = next(i for i in range(a, len(src)) if 'initialised = 1' in src[i])
    lines = _cpp(src[a:b], DEFS)
    py, ind = [], 0
    for l in lines:
        s = l.strip()
        if not s: continue
        m = re.match(r'for\((\w) = (0x[0-9A-Fa-f]+); \w <= (0x[0-9A-Fa-f]+); \w \+= (0x[0-9A-Fa-f]+)\)', s)
        lead = len(l) - len(l.lstrip('\t'))
        if m:
            py.append((lead, 'for %s in range(%s, %s+1, %s):' % m.groups()))
            continue
        m = re.match(r'JumpTable\[(.*)\] *= *CAST_OP\((\w+)\);', s)
        assert m, s
        py.append((lead, 'T[%s] = "%s"' % (m.group(1), m.group(2))))
    code, stack = [], []
    for lead, s in py:
        while stack and stack[-1] >= lead: stack.pop()
        code.append('    ' * len(stack) + s)
        if s.startswith('for'): stack.append(lead)
    T = [None] * 65536
    exec('\n'.join(code), {'T': T})
    return T

def handlers():
    src = _cpp(open(os.path.join(P, 'famec_opcodes.h')).read().split('\n'), DEFS)
    H, cur = {}, None
    for l in src:
        m = re.match(r'OPCODE\((\w+)\)', l)
        if m: cur = m.group(1); H[cur] = []; continue
        if cur: H[cur].append(l)
    return H

def rets(body):
    return re.findall(r'RET\(([^)]*\)?)\)', '\n'.join(body))

if __name__ == '__main__':
    T = jumptable(); H = handlers()
    used = sorted(set(T))
    print(len(used), 'handlers used')
    odd = [h for h in used if len(set(rets(H[h]))) != 1 or 'io_cycle_counter' in '\n'.join(H[h])]
    print(len(odd), 'with several RETs or counter changes')
