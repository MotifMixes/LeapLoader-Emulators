"""genspc.py OUT.S: the SPC700 core in asm for snes-hw milestone 6 (-DLL_SPCASM; replaces kern/llapu.S).

LLApuRunLE / LLApuRun2 / LLApuRunP as in llapu.S (same loop conditions), but the SPC700 registers live in ARC
registers and the common opcodes are asm handlers here; the others call the C handler (S9xApuOpcodes) with the
registers spilled to IAPU. Every handler does exactly what spc700.c / apumem.h do (same reads, writes, order of
I/O side effects, WaitAddress/WaitCounter/shutdown, cycles); IAPU.Address/Bit (scratch of the C core) are not kept.
Dispatch: one table word per opcode = handler offset | S9xAPUCycles[op] << 16 (cycles filled in at run time).
No lp loops, no gp-relative addressing.

Registers: r13 &IAPU, r14 CPU.APU_Cycles, r15 effective limit, r16 limit pointer, r17 PC (host pointer), r18 RAM,
r19 DirectPage, r20 A, r21 X, r22 Y, r23 S, r24 _Zero, r25 _Carry. Scratch r0-r7 in handlers (the slow memory
routines keep r1-r7 across their C calls), r8-r12 free.
"""
import re
import os
import sys

PC, RAM, DP, A, X, Y, S, Z, C = 'r17', 'r18', 'r19', 'r20', 'r21', 'r22', 'r23', 'r24', 'r25'
out = []
lab = [0]


def L():
    lab[0] += 1
    return '.Lq%d' % lab[0]


def e(*ls):
    for l in ls:
        out.append('\t' + l if not l.endswith(':') else l)


# ---------------------------------------------------------------- addressing: address into r1
def ea(mode):
    if mode == 'dp':
        e('ldb\tr1,[%s,1]' % PC)
    elif mode in ('dpx', 'dpy'):
        e('ldb\tr1,[%s,1]' % PC, 'add\tr1,r1,%s' % (X if mode == 'dpx' else Y), 'extb\tr1,r1')
    elif mode == 'ix':
        e('mov\tr1,%s' % X)
    elif mode == 'iy':
        e('mov\tr1,%s' % Y)
    elif mode == 'dp2':  # second operand byte as a dp address
        e('ldb\tr1,[%s,2]' % PC)
    elif mode in ('abs', 'absx', 'absy'):
        e('ldb\tr1,[%s,1]' % PC, 'ldb\tr2,[%s,2]' % PC, 'asl\tr2,r2,8', 'or\tr1,r1,r2')
        if mode != 'abs':
            e('add\tr1,r1,%s' % (X if mode == 'absx' else Y), 'bmsk\tr1,r1,15')
    elif mode == 'idx':  # [dp+X]: both bytes inside the direct page
        e('ldb\tr1,[%s,1]' % PC, 'add\tr1,r1,%s' % X, 'extb\tr2,r1', 'ldb\tr3,[%s,r2]' % DP,
          'add\tr1,r1,1', 'extb\tr2,r1', 'ldb\tr2,[%s,r2]' % DP, 'asl\tr2,r2,8', 'or\tr1,r3,r2')
    elif mode == 'idy':  # [dp]+Y: the high byte from DP+OP1+1 (no wrap, as the C core)
        e('ldb\tr1,[%s,1]' % PC, 'add\tr2,%s,r1' % DP, 'ldb\tr3,[r2]', 'ldb\tr2,[r2,1]', 'asl\tr2,r2,8',
          'or\tr1,r3,r2', 'add\tr1,r1,%s' % Y, 'bmsk\tr1,r1,15')
    else:
        raise ValueError(mode)


ZMODES = ('dp', 'dpx', 'dpy', 'ix', 'iy', 'dp2')
stubs = []


def rd(mode, dst='r0', addr_done=False):
    """value of the operand into dst (r0 by default); address stays in r1"""
    if mode == 'imm':
        e('ldb\t%s,[%s,1]' % (dst, PC))
        return
    if not addr_done:
        ea(mode)
    back = L()
    slow = L()
    if mode in ZMODES:
        e('brhs\tr1,0xf0,%s' % slow, 'ldb\tr0,[%s,r1]' % DP)
        stubs.append((slow, 'bl\t.LrdZs', back))
    else:
        e('sub\tr2,r1,0xf3', 'brlo\tr2,13,%s' % slow, 'ldb\tr0,[%s,r1]' % RAM)
        stubs.append((slow, 'bl\t.Lrds', back))
    e(back + ':')
    if dst != 'r0':
        e('mov\t%s,r0' % dst)


def wr(mode, val='r0'):
    """write val to the operand whose address is in r1"""
    if val != 'r0':
        e('mov\tr0,%s' % val)
    back = L()
    slow = L()
    if mode in ZMODES:
        e('brhs\tr1,0xf0,%s' % slow, 'add\tr2,%s,r1' % DP, 'stb\tr0,[r2]')
        stubs.append((slow, 'bl\t.LwrZs', back))
    else:
        e('sub\tr2,r1,0xf0', 'brlo\tr2,16,%s' % slow, 'brhs\tr1,0xffc0,%s' % slow, 'add\tr2,%s,r1' % RAM,
          'stb\tr0,[r2]')
        stubs.append((slow, 'bl\t.Lwrs', back))
    e(back + ':')


def pc(n):
    e('add\t%s,%s,%d' % (PC, PC, n))


def nxt():
    e('b\t.Lnext')


def wcinc():
    e('ld\tr2,[r13,IA_WC]', 'add\tr2,r2,1', 'st\tr2,[r13,IA_WC]')


# ---------------------------------------------------------------- ALU on (a, b) -> a (registers)
def alu(op, a, b):
    """a op= b (a, b registers holding bytes); flags as spc700.c. Uses r2-r5."""
    if op == 'or':
        e('or\t%s,%s,%s' % (a, a, b), 'mov\t%s,%s' % (Z, a))
    elif op == 'and':
        e('and\t%s,%s,%s' % (a, a, b), 'mov\t%s,%s' % (Z, a))
    elif op == 'eor':
        e('xor\t%s,%s,%s' % (a, a, b), 'mov\t%s,%s' % (Z, a))
    elif op == 'cmp':  # Int16 = a - b; C = Int16 >= 0; Z = (uint8) Int16
        e('sub\tr2,%s,%s' % (a, b), 'lsr\t%s,r2,31' % C, 'xor\t%s,%s,1' % (C, C), 'extb\t%s,r2' % Z)
    elif op == 'adc':
        e('add\tr2,%s,%s' % (a, b), 'add\tr2,r2,%s' % C,          # W = a + b + C
          'xor\tr3,%s,%s' % (a, b), 'xor\tr4,%s,r2' % b, 'bic\tr4,r4,r3',  # (b^W) & ~(a^b)
          'lsr\tr4,r4,7', 'and\tr4,r4,1', 'stb\tr4,[r13,IA_OVF]',
          'xor\tr3,r3,r2', 'and\tr3,r3,0x10', 'lsr\tr3,r3,1',     # H = (a^b^W) & 0x10
          'ldb\tr5,[r13,IA_P]', 'bclr\tr5,r5,3', 'or\tr5,r5,r3', 'stb\tr5,[r13,IA_P]',
          'lsr\t%s,r2,8' % C, 'extb\t%s,r2' % a, 'mov\t%s,%s' % (Z, a))
    elif op == 'sbc':
        e('sub\tr2,%s,%s' % (a, b), 'add\tr2,r2,%s' % C, 'sub\tr2,r2,1',   # Int16 = a - b + C - 1
          'lsr\t%s,r2,31' % C, 'xor\t%s,%s,1' % (C, C), 'extb\tr2,r2',
          'xor\tr3,%s,%s' % (a, b), 'xor\tr4,%s,r2' % a, 'and\tr4,r4,r3',  # (a^b) & (a^r) & 0x80
          'lsr\tr4,r4,7', 'and\tr4,r4,1', 'stb\tr4,[r13,IA_OVF]',
          'xor\tr3,r3,r2', 'and\tr3,r3,0x10', 'xor\tr3,r3,0x10', 'lsr\tr3,r3,1',   # H = !((a^b^r) & 0x10)
          'ldb\tr5,[r13,IA_P]', 'bclr\tr5,r5,3', 'or\tr5,r5,r3', 'stb\tr5,[r13,IA_P]',
          'mov\t%s,r2' % a, 'mov\t%s,r2' % Z)
    else:
        raise ValueError(op)


def shift(op, r):
    """ASL/ROL/LSR/ROR on register r (a byte)"""
    if op == 'asl':
        e('lsr\tr2,%s,7' % r, 'asl\t%s,%s,1' % (r, r), 'extb\t%s,%s' % (r, r), 'mov\t%s,r2' % C)
    elif op == 'rol':
        e('asl\tr2,%s,1' % r, 'or\tr2,r2,%s' % C, 'lsr\t%s,r2,8' % C, 'extb\t%s,r2' % r)
    elif op == 'lsr':
        e('and\t%s,%s,1' % (C, r), 'lsr\t%s,%s,1' % (r, r))
    elif op == 'ror':
        e('asl\tr2,%s,8' % C, 'or\tr2,r2,%s' % r, 'and\t%s,r2,1' % C, 'lsr\t%s,r2,1' % r)
    e('mov\t%s,%s' % (Z, r))


H = {}  # opcode -> handler emitter


def handler(*ops):
    def d(f):
        for o in ops:
            H[o] = (f, o)
        return f
    return d


# ---------------------------------------------------------------- ALU groups
ALU = {0x00: 'or', 0x20: 'and', 0x40: 'eor', 0x60: 'cmp', 0x80: 'adc', 0xA0: 'sbc'}
AMODE = {0x04: ('dp', 2), 0x05: ('abs', 3), 0x06: ('ix', 1), 0x07: ('idx', 2), 0x08: ('imm', 2),
         0x14: ('dpx', 2), 0x15: ('absx', 3), 0x16: ('absy', 3), 0x17: ('idy', 2)}
for base, name in ALU.items():
    for lo, (m, n) in AMODE.items():
        def f(o, name=name, m=m, n=n):
            rd(m)
            alu(name, A, 'r0')
            pc(n)
            nxt()
        H[base + lo] = (f, base + lo)

    def f9(o, name=name):  # dp(OP2) op= dp(OP1): reads OP1 then OP2
        rd('dp', 'r6')
        rd('dp2', 'r7')
        alu(name, 'r7', 'r6')
        if name != 'cmp':
            wr('dp2', 'r7')
        pc(3)
        nxt()
    H[base + 0x09] = (f9, base + 0x09)

    def f18(o, name=name):  # dp(OP2) op= #OP1
        e('ldb\tr6,[%s,1]' % PC)
        rd('dp2', 'r7')
        alu(name, 'r7', 'r6')
        if name != 'cmp':
            wr('dp2', 'r7')
        pc(3)
        nxt()
    H[base + 0x18] = (f18, base + 0x18)

    def f19(o, name=name):  # (X) op= (Y): reads (X) then (Y)
        rd('ix', 'r7')
        rd('iy', 'r6')
        alu(name, 'r7', 'r6')
        if name != 'cmp':
            e('mov\tr1,%s' % X)
            wr('ix', 'r7')
        pc(1)
        nxt()
    H[base + 0x19] = (f19, base + 0x19)

# CMP X / CMP Y
for o, (r, m, n) in {0xC8: (X, 'imm', 2), 0x3E: (X, 'dp', 2), 0x1E: (X, 'abs', 3),
                     0xAD: (Y, 'imm', 2), 0x7E: (Y, 'dp', 2), 0x5E: (Y, 'abs', 3)}.items():
    def f(o, r=r, m=m, n=n):
        rd(m)
        alu('cmp', r, 'r0')
        pc(n)
        nxt()
    H[o] = (f, o)

# shifts
for base, name in {0x00: 'asl', 0x20: 'rol', 0x40: 'lsr', 0x60: 'ror'}.items():
    for lo, (m, n) in {0x0B: ('dp', 2), 0x0C: ('abs', 3), 0x1B: ('dpx', 2)}.items():
        def f(o, name=name, m=m, n=n):
            rd(m, 'r6')
            shift(name, 'r6')
            wr(m, 'r6')
            pc(n)
            nxt()
        H[base + lo] = (f, base + lo)

    def fa(o, name=name):
        shift(name, A)
        pc(1)
        nxt()
    H[base + 0x1C] = (fa, base + 0x1C)

# INC / DEC (all count IAPU.WaitCounter)
for o, (d, m, n) in {0xAB: (1, 'dp', 2), 0xAC: (1, 'abs', 3), 0xBB: (1, 'dpx', 2),
                     0x8B: (-1, 'dp', 2), 0x8C: (-1, 'abs', 3), 0x9B: (-1, 'dpx', 2)}.items():
    def f(o, d=d, m=m, n=n):
        rd(m, 'r6')
        e('add\tr6,r6,%d' % d if d > 0 else 'sub\tr6,r6,1', 'extb\tr6,r6')
        wr(m, 'r6')
        e('mov\t%s,r6' % Z)
        wcinc()
        pc(n)
        nxt()
    H[o] = (f, o)
for o, (r, d) in {0xBC: (A, 1), 0x9C: (A, -1), 0x3D: (X, 1), 0x1D: (X, -1), 0xFC: (Y, 1), 0xDC: (Y, -1)}.items():
    def f(o, r=r, d=d):
        e('add\t%s,%s,1' % (r, r) if d > 0 else 'sub\t%s,%s,1' % (r, r), 'extb\t%s,%s' % (r, r), 'mov\t%s,%s' % (Z, r))
        wcinc()
        pc(1)
        nxt()
    H[o] = (f, o)

# MOV loads
for o, (r, m, n) in {0xE4: (A, 'dp', 2), 0xE5: (A, 'abs', 3), 0xE6: (A, 'ix', 1), 0xE7: (A, 'idx', 2),
                     0xE8: (A, 'imm', 2), 0xF4: (A, 'dpx', 2), 0xF5: (A, 'absx', 3), 0xF6: (A, 'absy', 3),
                     0xF7: (A, 'idy', 2), 0xCD: (X, 'imm', 2), 0xF8: (X, 'dp', 2), 0xF9: (X, 'dpy', 2),
                     0xE9: (X, 'abs', 3), 0x8D: (Y, 'imm', 2), 0xEB: (Y, 'dp', 2), 0xFB: (Y, 'dpx', 2),
                     0xEC: (Y, 'abs', 3)}.items():
    def f(o, r=r, m=m, n=n):
        rd(m)
        e('mov\t%s,r0' % r, 'mov\t%s,r0' % Z)
        pc(n)
        nxt()
    H[o] = (f, o)
# transfers
for o, (d, s_, fl) in {0x7D: (A, X, 1), 0xDD: (A, Y, 1), 0x5D: (X, A, 1), 0xFD: (Y, A, 1), 0x9D: (X, S, 1),
                       0xBD: (S, X, 0)}.items():
    def f(o, d=d, s_=s_, fl=fl):
        e('mov\t%s,%s' % (d, s_))
        if fl:
            e('mov\t%s,%s' % (Z, d))
        pc(1)
        nxt()
    H[o] = (f, o)
# MOV stores
for o, (r, m, n) in {0xC4: (A, 'dp', 2), 0xC5: (A, 'abs', 3), 0xC6: (A, 'ix', 1), 0xC7: (A, 'idx', 2),
                     0xD4: (A, 'dpx', 2), 0xD5: (A, 'absx', 3), 0xD6: (A, 'absy', 3), 0xD7: (A, 'idy', 2),
                     0xD8: (X, 'dp', 2), 0xD9: (X, 'dpy', 2), 0xC9: (X, 'abs', 3), 0xCB: (Y, 'dp', 2),
                     0xDB: (Y, 'dpx', 2), 0xCC: (Y, 'abs', 3)}.items():
    def f(o, r=r, m=m, n=n):
        ea(m)
        wr(m, r)
        pc(n)
        nxt()
    H[o] = (f, o)


@handler(0xAF)
def _(o):  # MOV (X)+,A
    e('mov\tr1,%s' % X, 'add\t%s,%s,1' % (X, X), 'extb\t%s,%s' % (X, X))
    wr('ix', A)
    pc(1)
    nxt()


@handler(0xBF)
def _(o):  # MOV A,(X)+
    e('mov\tr1,%s' % X, 'add\t%s,%s,1' % (X, X), 'extb\t%s,%s' % (X, X))
    rd('ix', addr_done=True)
    e('mov\t%s,r0' % A, 'mov\t%s,r0' % Z)
    pc(1)
    nxt()


@handler(0xFA)
def _(o):  # MOV dp(OP2),dp(OP1)
    rd('dp')
    e('ldb\tr1,[%s,2]' % PC)
    wr('dp2')
    pc(3)
    nxt()


@handler(0x8F)
def _(o):  # MOV dp(OP2),#OP1
    e('ldb\tr0,[%s,1]' % PC, 'ldb\tr1,[%s,2]' % PC)
    wr('dp2')
    pc(3)
    nxt()


# ---------------------------------------------------------------- branches
def rel(off, n):
    """r3 = RAM + ((PC - RAM + n + (int8) [PC+off]) & 0xffff)"""
    e('ldb.x\tr2,[%s,%d]' % (PC, off), 'sub\tr3,%s,%s' % (PC, RAM), 'add\tr3,r3,%d' % n, 'add\tr3,r3,r2',
      'bmsk\tr3,r3,15', 'add\tr3,r3,%s' % RAM)


def taken(shut):
    e('mov\t%s,r3' % PC, 'ld\tr2,[r13,IA_TWO]', 'add\tr14,r14,r2')
    if shut:
        e('bl\t.Lshut')
    nxt()


def cbranch(o, cond, shut):
    """cond: (instr, reg/bit) -> branch to .take when the branch is taken"""
    t = L()
    rel(1, 2)
    e(cond(t))
    pc(2)
    nxt()
    e(t + ':')
    taken(shut)


for o, (cond, shut) in {
        0x10: (lambda t: 'bbit0\t%s,7,%s' % (Z, t), 1), 0x30: (lambda t: 'bbit1\t%s,7,%s' % (Z, t), 1),
        0x90: (lambda t: 'breq\t%s,0,%s' % (C, t), 1), 0xB0: (lambda t: 'brne\t%s,0,%s' % (C, t), 1),
        0xD0: (lambda t: 'brne\t%s,0,%s' % (Z, t), 1), 0xF0: (lambda t: 'breq\t%s,0,%s' % (Z, t), 1)}.items():
    def f(o, cond=cond, shut=shut):
        cbranch(o, cond, shut)
    H[o] = (f, o)


@handler(0x50, 0x70)
def _(o):  # BVC / BVS
    t = L()
    rel(1, 2)
    e('ldb\tr2,[r13,IA_OVF]', ('breq' if o == 0x50 else 'brne') + '\tr2,0,%s' % t)
    pc(2)
    nxt()
    e(t + ':')
    taken(0)


@handler(0x2F)
def _(o):  # BRA (no extra cycles, no shutdown)
    rel(1, 2)
    e('mov\t%s,r3' % PC)
    nxt()


@handler(0x2E, 0xDE)
def _(o):  # CBNE dp / dp+X
    t = L()
    rd('dp' if o == 0x2E else 'dpx')
    rel(2, 3)
    e('brne\tr0,%s,%s' % (A, t))
    pc(3)
    nxt()
    e(t + ':')
    taken(1)


@handler(0x6E)
def _(o):  # DBNZ dp
    t = L()
    rd('dp', 'r6')
    e('sub\tr6,r6,1', 'extb\tr6,r6')
    wr('dp', 'r6')
    rel(2, 3)
    e('brne\tr6,0,%s' % t)
    pc(3)
    nxt()
    e(t + ':')
    taken(0)


@handler(0xFE)
def _(o):  # DBNZ Y
    t = L()
    rel(1, 2)
    e('sub\t%s,%s,1' % (Y, Y), 'extb\t%s,%s' % (Y, Y), 'brne\t%s,0,%s' % (Y, t))
    pc(2)
    nxt()
    e(t + ':')
    taken(0)


for b in range(8):
    for o, s_ in ((0x03 + 32 * b, 1), (0x13 + 32 * b, 0)):
        def f(o, b=b, s_=s_):
            t = L()
            rd('dp')
            rel(2, 3)
            e(('bbit1' if s_ else 'bbit0') + '\tr0,%d,%s' % (b, t))
            pc(3)
            nxt()
            e(t + ':')
            taken(0)
        H[o] = (f, o)
    for o, s_ in ((0x02 + 32 * b, 1), (0x12 + 32 * b, 0)):
        def f(o, b=b, s_=s_):
            rd('dp')
            e(('bset' if s_ else 'bclr') + '\tr0,r0,%d' % b)
            wr('dp')
            pc(2)
            nxt()
        H[o] = (f, o)


# ---------------------------------------------------------------- flags
@handler(0x60, 0x80, 0xED, 0x00)
def _(o):
    e({0x60: 'mov\t%s,0' % C, 0x80: 'mov\t%s,1' % C, 0xED: 'xor\t%s,%s,1' % (C, C), 0x00: 'nop'}[o])
    pc(1)
    nxt()


@handler(0x20, 0x40)
def _(o):  # CLRP / SETP
    e('ldb\tr2,[r13,IA_P]', ('bclr' if o == 0x20 else 'bset') + '\tr2,r2,5', 'stb\tr2,[r13,IA_P]')
    e('mov\t%s,%s' % (DP, RAM) if o == 0x20 else 'add\t%s,%s,0x100' % (DP, RAM))
    pc(1)
    nxt()


@handler(0xE0)
def _(o):  # CLRV: H and V cleared
    e('ldb\tr2,[r13,IA_P]', 'bclr\tr2,r2,3', 'stb\tr2,[r13,IA_P]', 'mov\tr2,0', 'stb\tr2,[r13,IA_OVF]')
    pc(1)
    nxt()


@handler(0xA0, 0xC0)
def _(o):  # EI / DI
    e('ldb\tr2,[r13,IA_P]', ('bset' if o == 0xA0 else 'bclr') + '\tr2,r2,2', 'stb\tr2,[r13,IA_P]')
    pc(1)
    nxt()


# ---------------------------------------------------------------- stack
def push(r):
    e('add\tr2,%s,%s' % (RAM, S), 'add\tr2,r2,0x100', 'stb\t%s,[r2]' % r, 'sub\t%s,%s,1' % (S, S), 'extb\t%s,%s' % (S, S))


def pushw(r):  # RAM[0xff+S] = lo, RAM[0x100+S] = hi, S -= 2
    e('add\tr2,%s,%s' % (RAM, S), 'add\tr2,r2,0x100', 'stb\t%s,[r2,-1]' % r, 'lsr\tr4,%s,8' % r, 'stb\tr4,[r2]',
      'sub\t%s,%s,2' % (S, S), 'extb\t%s,%s' % (S, S))


@handler(0x2D, 0x4D, 0x6D)
def _(o):
    push({0x2D: A, 0x4D: X, 0x6D: Y}[o])
    pc(1)
    nxt()


@handler(0xAE, 0xCE, 0xEE)
def _(o):
    r = {0xAE: A, 0xCE: X, 0xEE: Y}[o]
    e('add\t%s,%s,1' % (S, S), 'extb\t%s,%s' % (S, S), 'add\tr2,%s,%s' % (RAM, S), 'add\tr2,r2,0x100', 'ldb\t%s,[r2]' % r)
    pc(1)
    nxt()


@handler(0x0D)
def _(o):  # PUSH PSW: S9xAPUPackStatus (into IAPU.P) then push
    e('ldb\tr3,[r13,IA_P]', 'and\tr3,r3,0x3c',            # ~(Z|N|C|V)
      'or\tr3,r3,%s' % C,                                   # C is 0/1
      'ldb\tr2,[r13,IA_OVF]', 'breq\tr2,0,1f', 'bset\tr3,r3,6', '1:',
      'brne\t%s,0,2f' % Z, 'bset\tr3,r3,1', '2:',
      'and\tr2,%s,0x80' % Z, 'or\tr3,r3,r2', 'stb\tr3,[r13,IA_P]')
    push('r3')
    pc(1)
    nxt()


@handler(0x3F)
def _(o):  # CALL abs
    ea('abs')
    e('sub\tr3,%s,%s' % (PC, RAM), 'add\tr3,r3,3')
    pushw('r3')
    e('add\t%s,%s,r1' % (PC, RAM))
    nxt()


@handler(0x4F)
def _(o):  # PCALL
    e('ldb\tr1,[%s,1]' % PC, 'sub\tr3,%s,%s' % (PC, RAM), 'add\tr3,r3,2')
    pushw('r3')
    e('add\t%s,%s,r1' % (PC, RAM), 'add\t%s,%s,0xff00' % (PC, PC))
    nxt()


@handler(0x6F)
def _(o):  # RET
    e('add\t%s,%s,2' % (S, S), 'extb\t%s,%s' % (S, S), 'add\tr2,%s,%s' % (RAM, S), 'add\tr2,r2,0x100', 'ldb\tr3,[r2,-1]',
      'ldb\tr4,[r2]', 'asl\tr4,r4,8', 'or\tr3,r3,r4', 'add\t%s,%s,r3' % (PC, RAM))
    nxt()


@handler(0x5F)
def _(o):  # JMP abs
    ea('abs')
    e('add\t%s,%s,r1' % (PC, RAM))
    nxt()


@handler(0x1F)
def _(o):  # JMP [abs+X]: both bytes through S9xAPUGetByte
    ea('abs')
    e('add\tr1,r1,%s' % X, 'mov\tr7,r1', 'bmsk\tr1,r1,15')
    rd('abs', 'r6', addr_done=True)
    e('add\tr1,r7,1', 'bmsk\tr1,r1,15')
    rd('abs', addr_done=True)
    e('asl\tr0,r0,8', 'or\tr0,r0,r6', 'add\t%s,%s,r0' % (PC, RAM))
    nxt()


# ---------------------------------------------------------------- 16-bit and misc
def zn16(r):  # _Zero = ((w != 0) | (w >> 8)) & 0xff
    e('lsr\tr2,%s,8' % r, 'breq\t%s,0,1f' % r, 'or\tr2,r2,1', '1:', 'extb\t%s,r2' % Z)


def rdw():  # r6 = GetZ(OP1) | GetZ((OP1+1)&0xff) << 8 ; r7 = OP1
    rd('dp', 'r6')
    e('mov\tr7,r1', 'add\tr1,r1,1', 'extb\tr1,r1')
    rd('dp', addr_done=True)
    e('asl\tr0,r0,8', 'or\tr6,r6,r0')


@handler(0xBA)
def _(o):  # MOVW YA,dp
    rd('dp', A)
    e('add\tr1,r1,1', 'extb\tr1,r1')
    rd('dp', Y, addr_done=True)
    e('asl\tr3,%s,8' % Y, 'or\tr3,r3,%s' % A)
    zn16('r3')
    pc(2)
    nxt()


@handler(0xDA)
def _(o):  # MOVW dp,YA
    ea('dp')
    wr('dp', A)
    e('ldb\tr1,[%s,1]' % PC, 'add\tr1,r1,1', 'extb\tr1,r1')
    wr('dp', Y)
    pc(2)
    nxt()


@handler(0x1A, 0x3A)
def _(o):  # DECW / INCW
    rdw()
    e(('sub' if o == 0x1A else 'add') + '\tr6,r6,1', 'bmsk\tr6,r6,15', 'mov\tr1,r7', 'extb\tr0,r6')
    wr('dp')
    e('add\tr1,r7,1', 'extb\tr1,r1', 'lsr\tr0,r6,8')
    wr('dp')
    zn16('r6')
    pc(2)
    nxt()


def ya(r):
    e('asl\t%s,%s,8' % (r, Y), 'or\t%s,%s,%s' % (r, r, A))


def setya(r):
    e('extb\t%s,%s' % (A, r), 'lsr\t%s,%s,8' % (Y, r), 'extb\t%s,%s' % (Y, Y))


@handler(0x5A)
def _(o):  # CMPW YA,dp
    rdw()
    ya('r3')
    e('sub\tr3,r3,r6', 'lsr\t%s,r3,31' % C, 'xor\t%s,%s,1' % (C, C), 'bmsk\tr3,r3,15')
    zn16('r3')
    pc(2)
    nxt()


@handler(0x7A)
def _(o):  # ADDW YA,dp
    rdw()
    ya('r3')
    e('add\tr4,r3,r6',                                     # W32
      'lsr\t%s,r4,16' % C,
      'xor\tr5,r3,r6', 'xor\tr2,r6,r4', 'bic\tr2,r2,r5', 'lsr\tr2,r2,15', 'and\tr2,r2,1', 'stb\tr2,[r13,IA_OVF]',
      'xor\tr5,r5,r4', 'and\tr5,r5,0x10', 'lsr\tr5,r5,1',
      'ldb\tr2,[r13,IA_P]', 'bclr\tr2,r2,3', 'or\tr2,r2,r5', 'stb\tr2,[r13,IA_P]',
      'bmsk\tr4,r4,15')
    setya('r4')
    zn16('r4')
    pc(2)
    nxt()


@handler(0x9A)
def _(o):  # SUBW YA,dp
    rdw()
    ya('r3')
    e('sub\tr4,r3,r6', 'lsr\t%s,r4,31' % C, 'xor\t%s,%s,1' % (C, C), 'bmsk\tr4,r4,15',
      'xor\tr5,r3,r6', 'xor\tr2,r3,r4', 'and\tr2,r2,r5', 'lsr\tr2,r2,15', 'and\tr2,r2,1', 'stb\tr2,[r13,IA_OVF]',
      'xor\tr5,r5,r4', 'and\tr5,r5,0x10', 'xor\tr5,r5,0x10', 'lsr\tr5,r5,1',
      'ldb\tr2,[r13,IA_P]', 'bclr\tr2,r2,3', 'or\tr2,r2,r5', 'stb\tr2,[r13,IA_P]')
    setya('r4')
    zn16('r4')
    pc(2)
    nxt()


@handler(0xCF)
def _(o):  # MUL YA
    # exactly the C build's sequence (gcc 15 reads r58 (MMID) + extw for the uint16 product)
    e('mul64\t0,%s,%s' % (A, Y), 'mov\tr3,r58', 'extw\tr3,r3')
    setya('r3')
    zn16('r3')
    pc(1)
    nxt()


@handler(0x9F)
def _(o):  # XCN
    e('lsr\tr2,%s,4' % A, 'asl\tr3,%s,4' % A, 'or\tr2,r2,r3', 'extb\t%s,r2' % A, 'mov\t%s,%s' % (Z, A))
    pc(1)
    nxt()


@handler(0x0E, 0x4E)
def _(o):  # TSET1 / TCLR1 abs
    rd('abs', 'r6')
    e(('or' if o == 0x0E else 'bic') + '\tr0,r6,%s' % A)
    wr('abs')
    e('and\t%s,r6,%s' % (Z, A))
    pc(3)
    nxt()


# ---------------------------------------------------------------- output
def gen():
    import os
    hdr = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'genspc_hdr.S')).read() % \
        {'DP': DP, 'RAM': RAM, 'SHORT': SHORT}
    body = []
    global out
    for o in ORDER + [x for x in range(256) if x not in ORDER]:
        if o in H and o not in SKIP:
            out = []
            stubs.clear()
            f, oo = H[o]
            f(oo)
            for slow, call, back in stubs:
                out.append(slow + ':')
                out.append('\t' + call)
                out.append('\tb\t' + back)
            body.append('.Lh%02X:\t; %02X\n' % (o, o) + '\n'.join(out))
    tab = ['\t.data', '\t.align\t4', 'LLSpcTab:']
    for o in range(256):
        h = '.Lh%02X' % o if (o in H and o not in SKIP) else '.Lfb'   # B3 (-DLL_SPCDISP2): absolute addresses
        tab.append('#ifdef LL_SPCDISP2\n\t.word\t%s\n#else\n\t.word\t%s - .Lbase\n#endif' % (h, h))
    tab += ['#ifdef LL_SPCDISP2', 'LLSpcCyc:\t.space\t512', '#endif']
    tab += ['\t.global\tLLSpcTab', 'LLSpcLimP:\t.word\t0', 'LLSpcAdj:\t.word\t0', 'LLSpcMore:\t.word\t0', 'LLSpcPMode:\t.byte\t0', '\t.align\t4']
    txt = '\n'.join(body)
    if os.environ.get('SPCPEEP', '1') == '1':   # B3: "add PC,PC,n / b .Lnext" -> "b.d .Lnext / add PC,PC,n"
        txt = re.sub(r'\tadd\t%s,%s,(\d+)\n\tb\t\.Lnext\n' % (PC, PC), lambda m: '#ifdef LL_SPCDISP2\n\tb.d\t.Lnext\n\tadd\t%s,%s,%s\n#else\n\tadd\t%s,%s,%s\n\tb\t.Lnext\n#endif\n' % (PC, PC, m.group(1), PC, PC, m.group(1)), txt)
    return regmap(hdr + txt + '\n' + '\n'.join(tab) + '\n')


# source names -> final registers: the state that is cheap to reload goes to caller-saved r8-r12
# (no save/restore per call); PC DP A X Y S Z C to r13-r20 (saved by .Lenter)
RMAP = {'r13': 'r8', 'r14': 'r9', 'r15': 'r10', 'r16': 'r11', 'r18': 'r12', 'r17': 'r13', 'r19': 'r14',
        'r20': 'r15', 'r21': 'r16', 'r22': 'r17', 'r23': 'r18', 'r24': 'r19', 'r25': 'r20'}


def regmap(t):
    res = []
    for i, part in enumerate(re.split(r';NOMAP[{}]', t)):
        res.append(part if i % 2 else re.sub(r'\br(1[3-9]|2[0-5])\b', lambda m: RMAP[m.group(0)], part))
    return ''.join(res)


SKIP = set()
SHORT = 0
# env SPCHOT="hex ops" (core.sh with -DLL_SPCHOT): these handlers first, in this order (layout only)
ORDER = [int(t, 16) for t in __import__('os').environ.get('SPCHOT', '').split()]
if __name__ == '__main__':
    import os
    SHORT = int(os.environ.get('SPCSHORT', '100'))
    sk = os.environ.get('SPCSKIP', '')  # hex opcodes left to C (debugging)
    for t in sk.split():
        SKIP.add(int(t, 16))
    if os.environ.get('SPCONLY'):  # only these in asm
        keep = set(int(t, 16) for t in os.environ['SPCONLY'].split())
        SKIP |= set(range(256)) - keep
    open(sys.argv[1], 'w', newline='\n').write(gen())
    print('genspc: %d asm handlers, %d C' % (len([o for o in H if o not in SKIP]), 256 - len([o for o in H if o not in SKIP])))
