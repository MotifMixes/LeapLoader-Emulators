# parseops.py: helper for gen65.py: parses cpuops.c into {name: body-lines}, tables {mode: [names]}
import re, os
H = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(H, '..', 's9x', 'src', 'cpuops.c')
def strip_pp(body):
    # keep the VAR_CYCLES side of #ifdef, drop #ifndef SA1_OPCODES else branches (we are not SA1)
    out, stack = [], []
    for l in body.split('\n'):
        s = l.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|else|endif|elif)\s*(.*)', s)
        if m:
            k, a = m.group(1), m.group(2).strip()
            if k in ('ifdef', 'ifndef', 'if'):
                defs = {'VAR_CYCLES': 1, 'CPU_SHUTDOWN': 1, 'FAST_LSB_WORD_ACCESS': 0, 'SA1_OPCODES': 0,
                        'LL_SPEEDHACKS': 1, 'USE_SA1': 1, 'DEBUGGER': 0, 'LL_SLEEP': 1, 'LL_NOIDLE': 0}
                if k == 'if':
                    e = re.sub(r'defined\s*\(?\s*(\w+)\s*\)?', lambda m: str(defs.get(m.group(1), 0)), a)
                    e = e.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ')
                    v = eval(e)
                else:
                    v = defs.get(a.split()[0] if a else '', 0)
                if k == 'ifndef': v = not v
                stack.append(bool(v))
            elif k == 'else': stack[-1] = not stack[-1]
            elif k == 'endif': stack.pop()
            continue
        if all(stack): out.append(s)
    return [x for x in out if x]
def parse():
    c = open(SRC).read()
    funcs = {}
    for m in re.finditer(r'^(?:static\s+)?void\s+(Op\w+)\s*\(\s*(?:void)?\s*\)\s*\{(.*?)^\}', c, re.M | re.S):
        funcs[m.group(1)] = strip_pp(m.group(2))
    tabs = {}
    for mode in ('M1X1', 'M1X0', 'M0X1', 'M0X0'):
        m = re.search(r'SOpcodes S9xOpcodes%s\[256\]\s*=\s*\{(.*?)\};' % mode, c, re.S)
        tabs[mode] = re.findall(r'\{(\w+)\}', m.group(1))
    return funcs, tabs
if __name__ == '__main__':
    f, t = parse()
    odd = set()
    for mode in t:
        for n in t[mode]:
            b = f.get(n)
            if b is None: odd.add((n, 'MISSING')); continue
            if len(b) == 2 and all(re.match(r'\w+\(\);$', x) for x in b): continue
            odd.add((n, ' | '.join(b)[:150]))
    for n, b in sorted(odd): print(n, '::', b)
