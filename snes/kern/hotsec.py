"""hotsec.py LL65S HOTLIST: moves the 65816 handlers named in HOTLIST (one per line) of the generated ll65.S
into section .text.ll65hot and the other handlers into .text.ll65cold (layout only: every handler block
starts at its H_ label and ends with an unconditional branch, so blocks can be placed anywhere). The code
before the first handler (run loop, shared helpers) stays in .text. Used with port/order-*.txt."""
import re, sys
src, hot = sys.argv[1], set(open(sys.argv[2]).read().split())
lines = open(src).read().split('\n')
out, cur = [], None
i = 0
while i < len(lines):
    l = lines[i]
    m = re.match(r'(H_\w+):', l)
    nxt = re.match(r'(H_\w+):', lines[i + 1]) if l.strip() == '.align 2' and i + 1 < len(lines) else None
    if nxt or m:
        name = (nxt or m).group(1)
        sec = '.text.ll65hot' if name in hot else '.text.ll65cold'
        if sec != cur:
            out.append(f'\t.section {sec},"ax",@progbits')
            cur = sec
    out.append(l)
    i += 1
open(src, 'w').write('\n'.join(out))
print(sum(1 for l in out if l.startswith('\t.section')), 'section switches')
