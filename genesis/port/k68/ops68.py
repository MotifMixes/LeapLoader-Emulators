# ops68.py: the 68000 instruction templates of k68i.S (used by gen68.py).
# A handler serves one opcode >> 3 group: bits 3-15 of the opcode are known
# when the code is generated, bits 0-2 (usually the source register) are read
# from r17 at run time. EA mode 7 groups branch on bits 0-2 to one body per
# addressing variant.
import re
import fametab

T = fametab.jumptable()
H = fametab.handlers()
ILL = ('0x4AFC', '0xA000', '0xF000')

def legal(op):
    return T[op] not in ILL

def rets(op):
    """RET(n) values of FAME's handler for op, in source order"""
    out = []
    body = '\n'.join(H[T[op]])
    m = re.search(r'#define CYC (\d+)', body)
    body = re.sub(r'BITCOUNT\(res,[^;]*?\)(\*2)?(?=\))', '0', body)   # (counted at run time)
    for v in re.findall(r'RET\(([^()]*(?:\([^()]*\))?[^()]*)\)', body):
        if m: v = v.replace('CYC', m.group(1))
        try: out.append(eval(v))
        except Exception: out.append(None)
    return out

def cyc1(op):
    r = set(rets(op))
    body = '\n'.join(H[T[op]])
    if len(r) != 1 or 'io_cycle_counter' in body: return None
    return r.pop()

SZ = {1: 'b', 2: 'w', 4: 'l'}
LD = {1: 'ldb', 2: 'ldw', 4: 'ld'}
ST = {1: 'stb', 2: 'stw', 4: 'st'}

class Fail(Exception):
    pass

class G:
    """one handler being generated"""
    def __init__(self, op):
        self.op = op
        self.l = []
        self.nl = 0
        self.ext = 0       # extension bytes after the opcode (for the recompiler)
        self.have = set()  # 'S': r3 = register number of bits 0-2, 'D': r12 = offset of bits 9-11
        self.outline = (op >> 3) in OUTLINE_GRPS   # -DK68OUT: memory EAs via shared routines
    def e(self, *ss):
        for s in ss:
            if s.endswith(':'): self.have.clear()
            elif s.startswith(('bl\t', 'jl')):
                if not re.match(r'bl\tk68_(rd|wr)', s): self.have.clear()
                else: self.have.discard('S')
            else:
                m = re.match(r'\w+(\.\w+)?\t(r\d+),', s)
                if m and m.group(2) == 'r3': self.have.discard('S')
                if m and m.group(2) == 'r12': self.have.discard('D')
            self.l.append(s if s.endswith(':') else '\t' + s)
    def lab(self):
        self.nl += 1
        if self.nl > 99: raise Fail('labels')
        return self.nl
    def at(self, n):
        self.have.clear()
        self.l.append('%d:' % n)
    def next(self, cyc):
        if cyc is None: raise Fail('cycles')
        self.e('NEXT\t%d' % cyc)
    def nextr(self, reg):
        """charge cycles held in a register"""
        self.e('NEXTR\t%s' % reg)
    def text(self):
        return '\n'.join(self.l)

# ---------------------------------------------------------------- registers
# reg: an int (known register number) or 'S' (bits 0-2 of the opcode)
def idx(g, reg):
    """'S' (bits 0-2): register number in r3; 'D' (bits 9-11): byte offset in r12"""
    if reg in g.have: return
    if reg == 'S': g.e('and\tr3,r17,7')
    elif reg == 'D': g.e('lsr\tr12,r17,7', 'and\tr12,r12,0x1c')
    if reg in ('S', 'D'): g.have.add(reg)

def ld_dn(g, reg, d):
    if reg == 'S': idx(g, reg); g.e('ld.as\t%s,[r13,r3]' % d)
    elif reg == 'D': idx(g, reg); g.e('ld\t%s,[r13,r12]' % d)
    else: g.e('ld\t%s,[r13,%d]' % (d, reg * 4))

def ld_an(g, reg, d):
    if reg == 'S': idx(g, reg); g.e('ld.as\t%s,[r19,r3]' % d)
    elif reg == 'D': idx(g, reg); g.e('ld\t%s,[r19,r12]' % d)
    else: g.e('ld\t%s,[r19,%d]' % (d, reg * 4))

def st_dn(g, reg, v, sz=4):
    t = 'r1' if v == 'r2' else 'r2'
    if reg == 'S': idx(g, reg); g.e('add2\t%s,r13,r3' % t, '%s\t%s,[%s]' % (ST[sz], v, t))
    elif reg == 'D': idx(g, reg); g.e('add\t%s,r13,r12' % t, '%s\t%s,[%s]' % (ST[sz], v, t))
    else: g.e('%s\t%s,[r13,%d]' % (ST[sz], v, reg * 4))

def st_an(g, reg, v):
    t = 'r1' if v == 'r2' else 'r2'
    if reg == 'S': idx(g, reg); g.e('add2\t%s,r19,r3' % t, 'st\t%s,[%s]' % (v, t))
    elif reg == 'D': idx(g, reg); g.e('add\t%s,r19,r12' % t, 'st\t%s,[%s]' % (v, t))
    else: g.e('st\t%s,[r19,%d]' % (v, reg * 4))

# ---------------------------------------------------------------- outlined EAs (-DK68OUT)
OUTLINE_GRPS = set()
ROUTINES = {}       # routine text -> name

def outl(g, kind, ea, sz, v=None, dec=False):
    """emit 'bl' to a shared routine doing ea_addr (+ tail read / write); False if not worth it.
    kind 'a': address -> r0; 'r': value -> r0 (clobbers r0-r3); 'w': store v (clobbers r0-r3)"""
    g2 = G(0); g2.outline = False
    ea_addr(g2, ea, sz, 'r0')
    n = len(g2.l)
    if kind == 'a': g2.e('j	[blink]')
    elif kind == 'r': g2.e('b	k68_rd%d' % (sz * 8))
    else:
        if v != 'r1': g2.e('mov	r1,%s' % v)
        g2.e('b	k68_wr%d%s' % (sz * 8, 'd' if dec else ''))
    if n < (3 if kind == 'a' else 2): return False
    t = g2.text()
    if t not in ROUTINES:
        ROUTINES[t] = 'k68_ea%d_%s' % (len(ROUTINES), kind)
    g.ext += g2.ext
    g.e('bl	' + ROUTINES[t])
    return True

# ---------------------------------------------------------------- EAs
# ea = (mode, reg); mode 7: reg = 0 abs.w, 1 abs.l, 2 d16(pc), 3 d8(pc,xn), 4 #imm
def is_mem(ea):
    return ea[0] >= 2 and ea != (7, 4)

def fetch_ext(g, d, sz, signed=False):
    """immediate / extension data at PC"""
    g.ext += 4 if sz == 4 else 2
    if sz == 4:
        g.e('ldw\tr2,[r14,0]', 'ldw\t%s,[r14,2]' % d, 'asl\tr2,r2,16', 'or\t%s,%s,r2' % (d, d), 'add\tr14,r14,4')
    elif sz == 2:
        g.e('%s\t%s,[r14,0]' % ('ldw.x' if signed else 'ldw', d), 'add\tr14,r14,2')
    else:
        g.e('ldw\t%s,[r14,0]' % d, 'add\tr14,r14,2')
        g.e('%s\t%s,%s' % ('sexb' if signed else 'extb', d, d))

def index_ext(g, d):
    """d += d8 + Xn of the brief extension word at PC (d8(An,Xn), d8(PC,Xn))"""
    n = g.lab()
    g.e('ldw\tr1,[r14,0]', 'add\tr14,r14,2', 'sexb\tr2,r1', 'add\t%s,%s,r2' % (d, d),
        'lsr\tr2,r1,12', 'ld.as\tr2,[r13,r2]', 'bbit1\tr1,11,%df' % n, 'sexw\tr2,r2')
    g.at(n)
    g.e('add\t%s,%s,r2' % (d, d))

def ea_addr(g, ea, sz, d='r0'):
    """address of a memory EA into d (with (An)+ / -(An) updates); clobbers r1-r3"""
    if g.outline and d == 'r0' and outl(g, 'a', ea, sz): return
    m, r = ea
    if m in (5, 6) or ea in ((7, 0), (7, 2), (7, 3)): g.ext += 2
    if m == 2:
        ld_an(g, r, d)
    elif m == 3:
        ld_an(g, r, d)
        if sz == 1 and r in ('S', 'D'):
            g.e('mov\tr2,1', 'cmp\t%s,%d' % (('r3', 7) if r == 'S' else ('r12', 28)), 'mov.eq\tr2,2', 'add\tr1,%s,r2' % d)
        else:
            g.e('add\tr1,%s,%d' % (d, 2 if (sz == 1 and r == 7) else sz))
        st_an(g, r, 'r1')
    elif m == 4:
        ld_an(g, r, 'r1')
        if sz == 1 and r in ('S', 'D'):
            g.e('mov\tr2,1', 'cmp\t%s,%d' % (('r3', 7) if r == 'S' else ('r12', 28)), 'mov.eq\tr2,2', 'sub\t%s,r1,r2' % d)
        else:
            g.e('sub\t%s,r1,%d' % (d, 2 if (sz == 1 and r == 7) else sz))
        st_an(g, r, d)
    elif m == 5:
        g.e('ldw.x\tr1,[r14,0]', 'add\tr14,r14,2')
        ld_an(g, r, d)
        g.e('add\t%s,%s,r1' % (d, d))
    elif m == 6:
        ld_an(g, r, d)
        index_ext(g, d)
    elif ea == (7, 0):
        g.e('ldw.x\t%s,[r14,0]' % d, 'add\tr14,r14,2')
    elif ea == (7, 1):
        fetch_ext(g, d, 4)      # (counts 4)
    elif ea == (7, 2):
        g.e('ldw.x\tr1,[r14,0]', 'sub\t%s,r14,r15' % d, 'add\t%s,%s,r1' % (d, d), 'add\tr14,r14,2')
    elif ea == (7, 3):
        g.e('sub\t%s,r14,r15' % d)
        index_ext(g, d)
    else:
        raise Fail('ea_addr %s' % (ea,))

def rd(g, sz, d='r0'):
    """memory read at r0 -> d"""
    g.e('bl\tk68_rd%d' % (sz * 8))
    if d != 'r0': g.e('mov\t%s,r0' % d)

def wr(g, sz, a='r0', v='r1', dec=False):
    if v == 'r0' and a == 'r1': raise Fail('wr swap')
    if v != 'r1': g.e('mov\tr1,%s' % v)       # (first: v may be r0)
    if a != 'r0': g.e('mov\tr0,%s' % a)
    g.e('bl\tk68_wr%d%s' % (sz * 8, 'd' if dec else ''))

def ea_read(g, ea, sz, d, keep=None):
    """value of an EA (low sz bytes valid) into d; keep: register for the address"""
    m, r = ea
    if m == 0: ld_dn(g, r, d)
    elif m == 1: ld_an(g, r, d)
    elif ea == (7, 4): fetch_ext(g, d, sz)
    else:
        if g.outline and not keep and outl(g, 'r', ea, sz):
            if d != 'r0': g.e('mov\t%s,r0' % d)
            return
        ea_addr(g, ea, sz, 'r0')
        if keep: g.e('mov\t%s,r0' % keep)
        rd(g, sz, d)

def ea_write(g, ea, sz, v, a=None):
    """store v to an EA; memory EAs: address in register a (or computed here)"""
    m, r = ea
    if m == 0: st_dn(g, r, v, sz)
    elif m == 1: st_an(g, r, v)
    else:
        if a is None:
            if v in ('r0', 'r1', 'r2', 'r3'):
                g.e('mov\tr4,%s' % v); v = 'r4'
            if g.outline and v != 'r12' and outl(g, 'w', ea, sz, v, dec=(sz == 4 and m == 4 and g.dec_long)): return
            ea_addr(g, ea, sz, 'r0'); a = 'r0'
        wr(g, sz, a, v, dec=(sz == 4 and m == 4 and g.dec_long))

G.dec_long = False

# ---------------------------------------------------------------- flags
def sh(sz):
    return 32 - 8 * sz

def flags_logic(g, v, sz):
    """N, Z from v; V = C = 0; X kept"""
    if sz == 4: g.e('mov\tr20,%s' % v)
    else: g.e('asl\tr20,%s,%d' % (v, sh(sz)))
    g.e('and\tr22,r22,0x10')

def arith(g, kind, d, s, sz, res='r0'):
    """d (op) s with flags; kind add/sub/cmp; result (low sz bytes) -> res"""
    if sz != 4:
        g.e('asl\tr1,%s,%d' % (s, sh(sz)), 'asl\tr2,%s,%d' % (d, sh(sz)))
        s, d = 'r1', 'r2'
    if kind == 'cmp':
        g.e('and\tr22,r22,0x10', 'sub.f\tr20,%s,%s' % (d, s), 'or.cs\tr22,r22,1', 'or.vs\tr22,r22,2')
        return
    g.e('mov\tr22,0', '%s.f\tr20,%s,%s' % (kind, d, s), 'or.cs\tr22,r22,0x11', 'or.vs\tr22,r22,2')
    if res:
        if sz == 4: g.e('mov\t%s,r20' % res)
        else: g.e('lsr\t%s,r20,%d' % (res, sh(sz)))

def logic(g, kind, d, s, sz, res='r0'):
    g.e('%s\t%s,%s,%s' % ({'and': 'and', 'or': 'or', 'eor': 'xor'}[kind], res, d, s))
    flags_logic(g, res, sz)

def cond(g, cc, true_lab, neg=False):
    """branch to true_lab ('Nf') if condition cc holds (neg: if it fails); falls through otherwise"""
    if neg:
        n = g.lab(); cond(g, cc, '%df' % n); g.e('b\t%s' % true_lab); g.at(n); return
    t = true_lab
    if cc == 0: g.e('b\t%s' % t)
    elif cc == 1: pass
    elif cc == 2:
        n = g.lab(); g.e('bbit1\tr22,0,%df' % n, 'brne\tr20,0,%s' % t); g.at(n)
    elif cc == 3: g.e('bbit1\tr22,0,%s' % t, 'breq\tr20,0,%s' % t)
    elif cc == 4: g.e('bbit0\tr22,0,%s' % t)
    elif cc == 5: g.e('bbit1\tr22,0,%s' % t)
    elif cc == 6: g.e('brne\tr20,0,%s' % t)
    elif cc == 7: g.e('breq\tr20,0,%s' % t)
    elif cc == 8: g.e('bbit0\tr22,1,%s' % t)
    elif cc == 9: g.e('bbit1\tr22,1,%s' % t)
    elif cc == 10:
        n = g.lab(); g.e('brlt\tr20,0,%df' % n, 'bbit0\tr22,3,%s' % t); g.at(n)
    elif cc == 11: g.e('brlt\tr20,0,%s' % t, 'bbit1\tr22,3,%s' % t)
    else:
        g.e('lsr\tr0,r20,31', 'lsr\tr1,r22,3', 'or\tr0,r0,r1', 'lsr\tr1,r22,1', 'xor\tr0,r0,r1')
        if cc == 12: g.e('bbit0\tr0,0,%s' % t)
        elif cc == 13: g.e('bbit1\tr0,0,%s' % t)
        elif cc == 14:
            n = g.lab(); g.e('breq\tr20,0,%df' % n, 'bbit0\tr0,0,%s' % t); g.at(n)
        else: g.e('breq\tr20,0,%s' % t, 'bbit1\tr0,0,%s' % t)

def set_pc(g, r):
    """68k PC in r (not r2/r3) -> r14/r15 (FAME's SET_PC)"""
    g.e('lsr\tr2,%s,16' % r, 'bmsk\tr2,r2,7', 'add2\tr2,r13,r2', 'ld\tr2,[r2,0x8c]',
        'and\tr3,%s,0xff000000' % r, 'sub\tr15,r2,r3', 'add\tr14,%s,r15' % r)

def check_branch(g, r):
    """FAME's CHECK_BRANCH_EXCEPTION: odd target -> address error, end of slice"""
    g.e('btst\t%s,0' % r, 'bne\tk68_addrerr')

def push32(g, v):
    """A7 -= 4; write_long(A7, v); v not r0-r3"""
    g.e('ld\tr0,[r19,28]', 'sub\tr0,r0,4', 'st\tr0,[r19,28]', 'mov\tr1,%s' % v, 'bl\tk68_wr32')

# ---------------------------------------------------------------- generation driver
handlers = {}
table = None

labels = set()
templates = {}      # template text -> [id, ext bytes]
ttab = {}           # group -> template id, or list of 8 (mode 7 groups; None: FAME)

def template(text, ext):
    """recompiler template of a handler: NEXT -> TNEXT (charge, exit when
    the slice is over, else go on with the next instruction), absolute jumps"""
    t = re.sub(r'\tNEXT\t(\S+)', r'\tTNEXT\t\1', text)
    t = re.sub(r'\tNEXTR\t(\S+)', r'\tTNEXT\t\1', t)
    t = re.sub(r'\tbl\t(k68_\w+)', r'\tjl\t\1', t)
    t = re.sub(r'\tb(eq|ne)?\tk68_fallback', r'\tj\1\tk68_tfall', t)
    t = re.sub(r'\tb(eq|ne)?\t(k68_addrerr)', r'\tj\1\t\2', t)
    if re.search(r'\tb\w*\tk68_', t): raise Fail('template branch')
    key = (t, ext)
    if key not in templates:
        templates[key] = len(templates)
    return templates[key]

def put(grp, g, tpl=True):
    t = g.text()
    if t not in handlers:
        lab = 'h_%04x' % (grp << 3)
        while lab in labels: lab += 'x'
        labels.add(lab)
        handlers[t] = lab
    table[grp] = handlers[t]
    if tpl: ttab[grp] = template(t, g.ext)

def modes7(grp, allowed=(0, 1, 2, 3, 4)):
    """legal mode-7 variants of a group"""
    return [r for r in allowed if legal((grp << 3) | r)]

def group_ea(grp):
    op = grp << 3
    return (op >> 3) & 7

def gen_group(grp, body, ea_mode, allowed7=(0, 1, 2, 3, 4)):
    """body(g, ea, op) -> emits a handler body for one EA variant.
    ea_mode: the mode field of the group (EA in bits 0-5)."""
    op0 = grp << 3
    g = G(op0)
    if ea_mode != 7:
        ops = [op0 | r for r in range(8)]
        if not all(legal(o) for o in ops): return False
        body(g, (ea_mode, 'S'), op0)
        put(grp, g)
    else:
        rs = modes7(grp, allowed7)
        if not rs: return False
        tl = [None] * 8
        for r in rs:
            gt = G(op0 | r)
            body(gt, (7, r), op0 | r)
            tl[r] = template(gt.text(), gt.ext)
        labs = {}
        g.e('and\tr0,r17,7')
        for r in rs:
            labs[r] = g.lab()
            g.e('cmp\tr0,%d' % r, 'beq\t%df' % labs[r])
        g.e('b\tk68_fallback')
        for r in rs:
            g.at(labs[r])
            body(g, (7, r), op0 | r)
        put(grp, g, tpl=False)
        ttab[grp] = tl
    return True

def same_cyc(op, ea):
    """the FAME cycle count, the same for all 8 registers of a run-time-register group"""
    if ea[0] == 7: return cyc1(op)
    cs = set(cyc1((op & ~7) | r) for r in range(8))
    if len(cs) != 1: raise Fail('cycles differ %04x' % op)
    return cs.pop()

def try_gen(grp, body, mode, allowed7=(0, 1, 2, 3, 4)):
    try:
        return gen_group(grp, body, mode, allowed7)
    except Fail as e:
        fails.append('%04x: %s' % (grp << 3, e))
        return False

fails = []
