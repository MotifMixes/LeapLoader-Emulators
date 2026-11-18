# gen65.py (snes-hw milestone 2): writes s9x/src/ll65.S, the register-resident threaded 65816
# core for the ARC600 (-DLL_CORE2). Semantics follow snes9x2002's cpuops.c / cpuaddr.h /
# cpumacro.h / getset.h exactly (cycles, CPU.WaitAddress, LLWriteCount, idle table), so the
# emulation is the same as the C core's. Opcodes it does not know go to the C handler (LLcfb).
# No zero-overhead loops, no gp-relative addressing.
#
# r13 A  r14 X  r15 Y  r16 S  r17 D  r18 DB<<16  r19 PC (host pointer, opcode start until the
# dispatch)  r20 CPU.Cycles  r21 CPU.NextEvent (0x80000000 while CPU.Flags != 0: every
# instruction ends in LL65Between)  r22 dispatch table  r23 Z source (raw ICPU._Zero if <= 0xff,
# else raw 1)  r24 N source (raw ICPU._Negative = bits 8-15, N = bit 15)  r25 carry 0/1
# r27 CPU.MemSpeed  r12 cycles at the opcode start (LLOpCycles)
# Overflow, PL, PB, PCBase, WaitAddress live in the C structs.
# Scratch: r0 next opcode, r1 its handler, r2 address, r3 data; helpers (M_*) clobber r3-r5
# only; inline accesses clobber r4-r9; handler values live across accesses in r2, r3, r10, r11.
import os, re, sys
H = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, H)
import parseops

A, X, Y, S, D, DB, PC, CY, NE, TB, Z, N, C, MS, OC = ('r13', 'r14', 'r15', 'r16', 'r17', 'r18', 'r19',
    'r20', 'r21', 'r22', 'r23', 'r24', 'r25', 'r27', 'r12')
MIN = '0x80000000'
WA = '[CPU+C_WAITADDR]'
out = []
DEFER = []   # out-of-line slow paths of the current handler
lab = [0]
def L():
    lab[0] += 1
    return '.L%d' % lab[0]
def e(*ls):
    for l in ls:
        out.append(l if l.endswith(':') else '\t' + l)

def cyc(k=0, c=0):
    r = []
    if k == 1: r.append('add\tr20,r20,r27')
    elif k == 2: r.append('add1\tr20,r20,r27')
    elif k == 3: r += ['add\tr20,r20,r27', 'add1\tr20,r20,r27']
    elif k == 4: r.append('add2\tr20,r20,r27')
    elif k == 5: r += ['add\tr20,r20,r27', 'add2\tr20,r20,r27']
    elif k: raise ValueError(k)
    if c: r.append('add\tr20,r20,%d' % c)
    return r
def nz8(v):   # v holds the 8-bit result (0..255)
    return ['mov\tr23,%s' % v, 'asl\tr24,%s,8' % v] if v != Z else ['asl\tr24,r23,8']
def nz16(v):  # v holds the 16-bit result
    return ['mov\tr24,%s' % v, 'asl\tr23,%s,8' % v]
def op8(r, k=1): return ['ldb\t%s,[r19,%d]' % (r, k)]
def op16(r, t='r5'): return ['ldb\t%s,[r19,1]' % r, 'ldb\t%s,[r19,2]' % t, 'asl\t%s,%s,8' % (t, t), 'or\t%s,%s,%s' % (r, r, t)]
def op24(r, t='r5'):
    return ['ldb\t%s,[r19,1]' % r, 'ldb\t%s,[r19,2]' % t, 'asl\t%s,%s,8' % (t, t), 'or\t%s,%s,%s' % (r, r, t),
            'ldb\t%s,[r19,3]' % t, 'asl\t%s,%s,16' % (t, t), 'or\t%s,%s,%s' % (r, r, t)]

# ---- memory access: inline fast path for bank 0 $0000-$1FFF (WRAM, speed 8), else the helpers
def rd(w, fast):
    """read w (8/16) bits at r2 -> r3. fast: r2 is a 16-bit bank 0 address (direct page/stack).
    Default: the shared helpers (small handlers: the i-cache misses cost more than the calls);
    G65_INL=1: inline fast paths"""
    if not os.environ.get('G65_INL'):
        return ['bl	M_frd%d' % w] if fast else ['bl	M_rd%d' % w]
    if not fast:
        return rdg(w)
    s, d = L(), L()
    r = ['ld\tr4,[LL65Ram]', 'brhs\tr2,%s,%s' % ('0x2000' if w == 8 else '0x1fff', s), 'add\tr4,r4,r2', 'ldb\tr3,[r4,0]']
    if w == 16: r.append('ldb\tr5,[r4,1]')
    r += ['add\tr20,r20,%d' % (8 if w == 8 else 16), 'st\tr19,' + WA]
    if w == 16: r += ['asl\tr5,r5,8', 'or\tr3,r3,r5']
    r += [d + ':']
    DEFER.extend([s + ':', 'bl\tM_rd%d' % w, 'b\t' + d])
    return r
def wr(w, fast):
    """write w bits of r3 at r2 (helpers: clobber r0, r1, r3-r5)"""
    if not os.environ.get('G65_INL'):
        return ['bl	M_fwr%d' % w] if fast else ['bl	M_wr%d' % w]
    if not fast:
        return wrg(w)
    s, d = L(), L()
    r = ['ld\tr4,[LL65Ram]', 'brhs\tr2,%s,%s' % ('0x2000' if w == 8 else '0x1fff', s), 'add\tr4,r4,r2',
         'ld\tr6,[LLWriteCount]', 'ldb\tr5,[r4,0]']
    if w == 8:
        r += ['extb\tr7,r3', 'stb\tr3,[r4,0]', 'mov\tr8,0', 'add\tr20,r20,8', 'st\tr8,' + WA, 'cmp\tr5,r7']
    else:
        r += ['ldb\tr9,[r4,1]', 'extb\tr7,r3', 'lsr\tr8,r3,8', 'stb\tr3,[r4,0]', 'stb\tr8,[r4,1]', 'xor\tr5,r5,r7',
              'xor\tr9,r9,r8', 'mov\tr8,0', 'add\tr20,r20,16', 'st\tr8,' + WA, 'or.f\t0,r5,r9']
    r += ['add.ne\tr6,r6,1', 'st\tr6,[LLWriteCount]', d + ':']
    DEFER.extend([s + ':', 'bl\tM_wr%d' % w, 'b\t' + d])
    return r
def rdg(w):
    """inline generic read of r2 through LL65Map (memory blocks); I/O and $xx1FFF words: M_rd*"""
    s, d = L(), L()
    r = ['lsr\tr4,r2,8', 'and\tr4,r4,0xfff0', 'ld\tr5,[r4,LL65Map]', 'ld\tr3,[r4,LL65Map+8]', 'ld\tr4,[r4,LL65Map+12]']
    if w == 16: r += ['bmsk\tr6,r2,12', 'breq\tr6,0x1fff,' + s]
    r += ['brlo\tr5,MAPLAST,' + s, 'bmsk\tr6,r2,15']
    if w == 8:
        r += ['add\tr20,r20,r3', 'ldb\tr3,[r5,r6]', 'st\tr19,[r4,0]']
    else:
        r += ['add\tr5,r5,r6', 'add1\tr20,r20,r3', 'ldb\tr3,[r5,0]', 'ldb\tr5,[r5,1]', 'st\tr19,[r4,0]', 'nop', 'asl\tr5,r5,8', 'or\tr3,r3,r5']
    r.append(d + ':')
    DEFER.extend([s + ':', 'bl\tM_rd%d' % w, 'b\t' + d])
    return r
def wrg(w):
    """inline generic write of r3 at r2 (S9xSetByte/S9xSetWord on memory blocks)"""
    s, d = L(), L()
    r = ['lsr\tr4,r2,8', 'and\tr4,r4,0xfff0', 'ld\tr5,[r4,LL65Map+4]', 'ld\tr4,[r4,LL65Map+8]', 'ld\tr6,[LLWriteCount]',
         'brlo\tr5,MAPLAST,' + s, 'bmsk\tr7,r2,15']
    if w == 8:
        r += ['add\tr5,r5,r7', 'add\tr20,r20,r4', 'ldb\tr7,[r5,0]', 'extb\tr8,r3', 'stb\tr3,[r5,0]', 'mov\tr4,0', 'st\tr4,' + WA, 'cmp\tr7,r8']
    else:
        r += ['add\tr8,r7,1', 'bmsk\tr8,r8,15', 'add\tr7,r5,r7', 'add\tr8,r5,r8', 'add1\tr20,r20,r4', 'ldb\tr4,[r7,0]', 'ldb\tr9,[r8,0]',
              'extb\tr5,r3', 'stb\tr3,[r7,0]', 'lsr\tr7,r3,8', 'stb\tr7,[r8,0]', 'xor\tr4,r4,r5', 'xor\tr9,r9,r7', 'mov\tr5,0',
              'st\tr5,' + WA, 'or.f\t0,r4,r9']
    r += ['add.ne\tr6,r6,1', 'st\tr6,[LLWriteCount]', d + ':']
    DEFER.extend([s + ':', 'bl\tM_wr%d' % w, 'b\t' + d])
    return r
def push8(v):   # PushB(v): SetByte(v, S--)
    return ['mov\tr2,r16'] + (['mov\tr3,%s' % v] if v != 'r3' else []) + wr(8, True) + ['sub\tr16,r16,1', 'bmsk\tr16,r16,15']
def push16(v):  # PushW(v): SetWord(v, S - 1) (int: S = 0 gives -1); S -= 2
    return ['sub\tr2,r16,1'] + (['mov\tr3,%s' % v] if v != 'r3' else []) + wr(16, True) + ['sub\tr16,r16,2', 'bmsk\tr16,r16,15']
def pull8():    # GetByte(++S)
    return ['add\tr16,r16,1', 'bmsk\tr16,r16,15', 'mov\tr2,r16'] + rd(8, True)
def pull16():   # GetWord(S + 1) (int, may be 0x10000); S += 2
    return ['add\tr2,r16,1'] + rd(16, True) + ['add\tr16,r16,2', 'bmsk\tr16,r16,15']

# ---- addressing modes (cpuaddr.h): code leaving the address in r2, its length, fast flag.
# Cycles: the opcode fetch (C loop) + the mode's own, in C's order.
def ea(mode, w):
    if mode == 'Absolute':
        return op16('r2') + cyc(3) + ['add\tr2,r2,r18'], 3, False
    if mode == 'AbsoluteIndexedX':
        return op16('r2') + cyc(3) + ['add\tr2,r2,r18', 'add\tr2,r2,r14'], 3, False
    if mode == 'AbsoluteIndexedY':
        return op16('r2') + cyc(3) + ['add\tr2,r2,r18', 'add\tr2,r2,r15'], 3, False
    if mode == 'AbsoluteLong':
        return op24('r2') + cyc(4), 4, False
    if mode == 'AbsoluteLongIndexedX':
        return op24('r2') + cyc(4) + ['add\tr2,r2,r14', 'bmsk\tr2,r2,23'], 4, False
    if mode == 'Direct':
        return op8('r2') + cyc(2) + ['add\tr2,r2,r17', 'bmsk\tr2,r2,15'], 2, True
    if mode == 'DirectIndexedX':
        return op8('r2') + cyc(2, 6) + ['add\tr2,r2,r17', 'add\tr2,r2,r14', 'bmsk\tr2,r2,15'], 2, True
    if mode == 'DirectIndexedY':
        return op8('r2') + cyc(2, 6) + ['add\tr2,r2,r17', 'add\tr2,r2,r15', 'bmsk\tr2,r2,15'], 2, True
    if mode == 'StackRelative':
        return op8('r2') + cyc(2, 6) + ['add\tr2,r2,r16', 'bmsk\tr2,r2,15'], 2, True
    dp = op8('r2') + cyc(2) + ['add\tr2,r2,r17', 'bmsk\tr2,r2,15']
    if mode == 'DirectIndirect':
        return dp + rd(16, True) + ['add\tr2,r3,r18'], 2, False
    if mode == 'DirectIndirectIndexed':
        return dp + rd(16, True) + ['add\tr2,r3,r18', 'add\tr2,r2,r15'], 2, False
    if mode == 'DirectIndexedIndirect':
        return (op8('r2') + cyc(2) + ['add\tr2,r2,r17', 'add\tr2,r2,r14', 'bmsk\tr2,r2,15'] + rd(16, True) +
                ['add\tr2,r3,r18'] + cyc(0, 6)), 2, False
    if mode in ('DirectIndirectLong', 'DirectIndirectIndexedLong'):
        # GetWord(t) + (GetByte(t + 2) << 16) (+ Y)
        r = dp + rd(16, True) + ['mov\tr10,r3', 'add\tr2,r2,2'] + rd(8, False) + ['asl\tr3,r3,16', 'add\tr2,r10,r3']
        if mode == 'DirectIndirectIndexedLong': r.append('add\tr2,r2,r15')
        return r, 2, False
    if mode == 'StackRelativeIndirectIndexed':
        return (op8('r2') + cyc(2, 12) + ['add\tr2,r2,r16', 'bmsk\tr2,r2,15'] + rd(16, True) +
                ['add\tr2,r3,r18', 'add\tr2,r2,r15', 'bmsk\tr2,r2,23']), 2, False
    if mode in ('Immediate8', 'Immediate16'):
        # OpAddress = ShiftedPB + PC - PCBase (read through the memory map by the op)
        return ['ld\tr4,[CPU+C_PCBASE]', 'ld\tr2,[ICPU+I_SPB]'] + cyc(1) + ['add\tr5,r19,1', 'sub\tr5,r5,r4', 'add\tr2,r2,r5'], \
            (2 if mode == 'Immediate8' else 3), False
    raise KeyError(mode)

def alu8_merge(v):  # AL = v (v <= 0xff), keep B
    return ['and\tr13,r13,0xff00', 'or\tr13,r13,%s' % v]

# ---- operations (cpumacro.h) with the address in r2; returns (code, writes)
def oper(op, fast):
    w = 16 if op.endswith('16') else 8
    o = op[:-2] if w == 16 else op[:-1]
    if o in ('LDA', 'LDX', 'LDY'):
        if o == 'LDA':
            return rd(w, fast) + (['mov\tr13,r3'] + nz16('r3') if w == 16 else alu8_merge('r3') + nz8('r3')), False
        reg = X if o == 'LDX' else Y
        return rd(w, fast) + ['mov\t%s,r3' % reg] + (nz16('r3') if w == 16 else nz8('r3')), False
    if o in ('STA', 'STX', 'STY', 'STZ'):
        src = {'STA': A, 'STX': X, 'STY': Y}.get(o)
        return (['mov\tr3,%s' % src] if src else ['mov\tr3,0']) + wr(w, fast), True
    if o in ('AND', 'ORA', 'EOR'):
        if w == 16:
            ins = {'AND': 'and', 'ORA': 'or', 'EOR': 'xor'}[o]
            return rd(w, fast) + ['%s\tr13,r13,r3' % ins] + nz16('r13'), False
        if o == 'AND': return rd(w, fast) + ['or\tr3,r3,0xff00', 'and\tr13,r13,r3', 'bmsk\tr3,r13,7'] + nz8('r3'), False
        if o == 'ORA': return rd(w, fast) + ['or\tr13,r13,r3', 'bmsk\tr3,r13,7'] + nz8('r3'), False
        return rd(w, fast) + ['xor\tr13,r13,r3', 'bmsk\tr3,r13,7'] + nz8('r3'), False
    if o == 'ADC':
        if w == 8:
            return rd(w, fast) + ['bmsk\tr4,r13,7', 'add\tr5,r4,r3', 'add\tr5,r5,r25', 'xor\tr6,r4,r3', 'xor\tr7,r3,r5', 'lsr\tr25,r5,8',
                        'bic\tr7,r7,r6', 'bmsk\tr5,r5,7', 'lsr\tr7,r7,7', 'and\tr7,r7,1', 'stb\tr7,[ICPU+I_OVF]'] + alu8_merge('r5') + nz8('r5'), False
        return rd(w, fast) + ['add\tr5,r13,r3', 'add\tr5,r5,r25', 'xor\tr6,r13,r3', 'xor\tr7,r3,r5', 'lsr\tr25,r5,16',
                    'bic\tr7,r7,r6', 'bmsk\tr13,r5,15', 'lsr\tr7,r7,15', 'and\tr7,r7,1', 'stb\tr7,[ICPU+I_OVF]'] + nz16('r13'), False
    if o == 'SBC':
        if w == 8:
            return rd(w, fast) + ['bmsk\tr4,r13,7', 'sub\tr5,r4,r3', 'add\tr5,r5,r25', 'sub\tr5,r5,1', 'xor\tr6,r4,r3', 'lsr\tr25,r5,31',
                        'bmsk\tr5,r5,7', 'xor\tr25,r25,1', 'xor\tr7,r4,r5', 'and\tr7,r7,r6', 'lsr\tr7,r7,7', 'and\tr7,r7,1',
                        'stb\tr7,[ICPU+I_OVF]'] + alu8_merge('r5') + nz8('r5'), False
        return rd(w, fast) + ['sub\tr5,r13,r3', 'add\tr5,r5,r25', 'sub\tr5,r5,1', 'xor\tr6,r13,r3', 'lsr\tr25,r5,31', 'bmsk\tr5,r5,15',
                    'xor\tr25,r25,1', 'xor\tr7,r13,r5', 'and\tr7,r7,r6', 'mov\tr13,r5', 'lsr\tr7,r7,15', 'and\tr7,r7,1',
                    'stb\tr7,[ICPU+I_OVF]'] + nz16('r13'), False
    if o in ('CMP', 'CMX', 'CMY'):
        reg = {'CMP': A, 'CMX': X, 'CMY': Y}[o]
        if w == 8:
            return rd(w, fast) + ['bmsk\tr4,%s,7' % reg, 'sub\tr5,r4,r3', 'lsr\tr25,r5,31', 'bmsk\tr5,r5,7', 'xor\tr25,r25,1'] + nz8('r5'), False
        return rd(w, fast) + ['sub\tr5,%s,r3' % reg, 'lsr\tr25,r5,31', 'bmsk\tr5,r5,15', 'xor\tr25,r25,1'] + nz16('r5'), False
    if o == 'BIT':
        if w == 8:
            return rd(w, fast) + ['lsr\tr4,r3,6', 'and\tr4,r4,1', 'stb\tr4,[ICPU+I_OVF]', 'asl\tr24,r3,8', 'and\tr23,r3,r13'], False
        return rd(w, fast) + ['lsr\tr4,r3,14', 'and\tr4,r4,1', 'stb\tr4,[ICPU+I_OVF]', 'mov\tr24,r3', 'and\tr23,r3,r13', 'asl\tr23,r23,8'], False
    # read-modify-write: Cycles += ONE_CYCLE first
    m = 0xff if w == 8 else 0xffff
    top = 7 if w == 8 else 15
    nz = nz8 if w == 8 else nz16
    pre = cyc(0, 6) + rd(w, fast)
    if o in ('INC', 'DEC'):
        return pre + ['%s\tr3,r3,1' % ('add' if o == 'INC' else 'sub'), 'bmsk\tr3,r3,%d' % top, 'mov\tr10,r3'] + wr(w, fast) + nz('r10'), True
    if o == 'ASL':
        return pre + ['lsr\tr25,r3,%d' % top, 'asl\tr3,r3,1', 'bmsk\tr3,r3,%d' % top, 'mov\tr10,r3'] + wr(w, fast) + nz('r10'), True
    if o == 'LSR':
        return pre + ['and\tr25,r3,1', 'lsr\tr3,r3,1', 'mov\tr10,r3'] + wr(w, fast) + nz('r10'), True
    if o == 'ROL':
        return pre + ['asl\tr3,r3,1', 'or\tr3,r3,r25', 'lsr\tr25,r3,%d' % (top + 1), 'bmsk\tr3,r3,%d' % top, 'mov\tr10,r3'] + wr(w, fast) + nz('r10'), True
    if o == 'ROR':
        return pre + ['asl\tr4,r25,%d' % (top + 1), 'or\tr3,r3,r4', 'and\tr25,r3,1', 'lsr\tr3,r3,1', 'mov\tr10,r3'] + wr(w, fast) + nz('r10'), True
    if o in ('TSB', 'TRB'):
        z = ['and\tr23,r3,r13'] + (['asl\tr23,r23,8'] if w == 16 else [])
        f = ['or\tr3,r3,r13', 'bmsk\tr3,r3,%d' % top] if o == 'TSB' else ['bic\tr3,r3,r13']
        return pre + z + f + wr(w, fast), True
    raise KeyError(op)

# ---- the end of a handler: dispatch of the next instruction (threaded, no shared loop)
def tail(n, early=False, jump=False):
    """n: instruction length (PC += n); jump: r19 already holds the next PC, r11 the opcode start"""
    r = []
    if not os.environ.get('G65_OWNTAIL') and not early:
        return ['b	LLtJ' if jump else 'b	LLt%d' % n]   # shared dispatch tails (i-cache)
    if jump:
        r += ['ldb\tr0,[r19,0]', 'nop', 'nop', 'ld.as\tr1,[r22,r0]', 'nop']
        s = L()
        return r + ['brge\tr20,r21,%s' % s, 'j.d\t[r1]', 'mov\tr12,r20', s + ':', 'b\tLLevJ']
    if not early:
        r += ['ldb\tr0,[r19,%d]' % n, 'add\tr19,r19,%d' % n, 'nop', 'ld.as\tr1,[r22,r0]', 'nop']
    else:
        r += ['add\tr19,r19,%d' % n]
    s = L()
    return r + ['brge\tr20,r21,%s' % s, 'j.d\t[r1]', 'mov\tr12,r20', s + ':', 'b\tLLev%d' % n]

def early_body(n, body):
    """the next opcode fetched before the body (no memory writes in it): ldb first, ld.as after 2 more"""
    r = ['ldb\tr0,[r19,%d]' % n]
    b = list(body)
    # place the table load after the first two real instructions, but never after a label or a call
    k = 0
    pos = None
    for i, l in enumerate(b):
        if l.endswith(':') or l.startswith('bl') or l.startswith('b\t') or l.startswith('br') or l.startswith('j'):
            break
        k += 1
        if k == 2:
            pos = i + 1
            break
    if pos is None:
        return r + ['nop', 'nop', 'ld.as\tr1,[r22,r0]'] + b
    return r + b[:pos] + ['ld.as\tr1,[r22,r0]'] + b[pos:]

# ---- handlers

def two_call(name, mode, op):
    w = 16 if op.endswith('16') else 8
    code, n, fast = ea(mode, w)
    body, writes = oper(op, fast)
    pre = []
    if op[:3] in ('ADC', 'SBC'):
        pre = ['ldb\tr4,[CPU+C_PL]', 'nop', 'nop', 'bbit1\tr4,3,FARCFB']   # decimal mode: the C handler
    if writes:
        return pre + code + body + tail(n)
    return pre + early_body(n, code + body) + tail(n, early=True)

def imm(name):
    """the immediate ops that read *CPU.PC themselves"""
    m = re.match(r'Op([0-9A-F]{2})([MX])([01])$', name)
    if not m: return None
    oc, f, v = m.group(1), m.group(2), m.group(3)
    w = 8 if v == '1' else 16
    n = 2 if w == 8 else 3
    get = op8('r3') if w == 8 else op16('r3')
    c = cyc(2 if w == 8 else 3)
    nz = nz8 if w == 8 else nz16
    if oc in ('A9', 'A2', 'A0'):      # LDA LDX LDY #
        if oc == 'A9':
            body = get + c + ((['mov\tr13,r3'] + nz16('r3')) if w == 16 else alu8_merge('r3') + nz8('r3'))
        else:
            body = get + c + ['mov\t%s,r3' % (X if oc == 'A2' else Y)] + nz('r3')
    elif oc in ('29', '09', '49'):    # AND ORA EOR #
        ins = {'29': 'and', '09': 'or', '49': 'xor'}[oc]
        if w == 16: body = get + c + ['%s\tr13,r13,r3' % ins] + nz16('r13')
        elif oc == '29': body = get + c + ['or\tr3,r3,0xff00', 'and\tr13,r13,r3', 'bmsk\tr3,r13,7'] + nz8('r3')
        else: body = get + c + ['%s\tr13,r13,r3' % ins, 'bmsk\tr3,r13,7'] + nz8('r3')
    elif oc in ('C9', 'E0', 'C0'):    # CMP CPX CPY #
        reg = {'C9': A, 'E0': X, 'C0': Y}[oc]
        if w == 8: body = get + c + ['bmsk\tr4,%s,7' % reg, 'sub\tr5,r4,r3', 'lsr\tr25,r5,31', 'bmsk\tr5,r5,7', 'xor\tr25,r25,1'] + nz8('r5')
        else: body = get + c + ['sub\tr5,%s,r3' % reg, 'lsr\tr25,r5,31', 'bmsk\tr5,r5,15', 'xor\tr25,r25,1'] + nz16('r5')
    elif oc == '89':                  # BIT #: Z only
        body = get + c + (['and\tr23,r13,r3'] if w == 8 else ['and\tr23,r13,r3', 'asl\tr23,r23,8'])
    else:
        return None
    return early_body(n, body) + tail(n, early=True)

def implied(name):
    """one-byte ops: transfers, inc/dec, flags, accumulator shifts"""
    m = re.match(r'Op([0-9A-F]{2})([MX][01])?$', name)
    if not m: return None
    oc, md = m.group(1), m.group(2)
    w8 = md is not None and md[1] == '1'
    nz = nz8 if w8 else nz16
    top = 7 if w8 else 15
    one = cyc(1, 6)
    zero_wa = ['mov\tr4,0', 'st\tr4,' + WA]
    def tr(dst, src):          # transfer (8: low byte only into a 16-bit register)
        if dst == A and w8: return alu8_merge_from(src) + nz8('r3')
        if w8: return ['bmsk\t%s,%s,7' % (dst, src)] + nz8(dst)
        return ['mov\t%s,%s' % (dst, src)] + nz16(dst)
    def alu8_merge_from(src): return ['bmsk\tr3,%s,7' % src] + alu8_merge('r3')
    b = None
    if oc == 'AA': b = one + tr(X, A)            # TAX
    elif oc == 'A8': b = one + tr(Y, A)          # TAY
    elif oc == '8A': b = one + tr(A, X)          # TXA
    elif oc == '98': b = one + tr(A, Y)          # TYA
    elif oc == '9B': b = one + tr(Y, X)          # TXY
    elif oc == 'BB': b = one + tr(X, Y)          # TYX
    elif oc == 'BA': b = one + tr(X, S)          # TSX
    elif oc == '9A' and md is None: b = one + ['mov\tr16,r14']   # TXS (16 bits)
    elif oc == '1B' and md is None: b = one + ['mov\tr16,r13']   # TCS
    elif oc == '3B' and md is None: b = one + ['mov\tr13,r16'] + nz16('r13')   # TSC
    elif oc == '5B' and md is None: b = one + ['mov\tr17,r13'] + nz16('r17')   # TCD
    elif oc == '7B' and md is None: b = one + ['mov\tr13,r17'] + nz16('r13')   # TDC
    elif oc in ('E8', 'CA', 'C8', '88'):         # INX DEX INY DEY
        reg = X if oc in ('E8', 'CA') else Y
        b = one + zero_wa + ['%s\t%s,%s,1' % ('add' if oc in ('E8', 'C8') else 'sub', reg, reg), 'bmsk\t%s,%s,%d' % (reg, reg, top)] + nz(reg)
    elif oc in ('1A', '3A'):                     # INC A, DEC A
        ins = 'add' if oc == '1A' else 'sub'
        if w8: b = one + zero_wa + ['%s\tr3,r13,1' % ins, 'bmsk\tr3,r3,7'] + alu8_merge('r3') + nz8('r3')
        else: b = one + zero_wa + ['%s\tr13,r13,1' % ins, 'bmsk\tr13,r13,15'] + nz16('r13')
    elif oc == '0A':                             # ASL A
        if w8: b = one + ['bmsk\tr3,r13,7', 'lsr\tr25,r3,7', 'asl\tr3,r3,1', 'bmsk\tr3,r3,7'] + alu8_merge('r3') + nz8('r3')
        else: b = one + ['lsr\tr25,r13,15', 'asl\tr13,r13,1', 'bmsk\tr13,r13,15'] + nz16('r13')
    elif oc == '4A':                             # LSR A
        if w8: b = one + ['bmsk\tr3,r13,7', 'and\tr25,r3,1', 'lsr\tr3,r3,1'] + alu8_merge('r3') + nz8('r3')
        else: b = one + ['and\tr25,r13,1', 'lsr\tr13,r13,1'] + nz16('r13')
    elif oc == '2A':                             # ROL A
        if w8: b = one + ['bmsk\tr3,r13,7', 'asl\tr3,r3,1', 'or\tr3,r3,r25', 'lsr\tr25,r3,8', 'bmsk\tr3,r3,7'] + alu8_merge('r3') + nz8('r3')
        else: b = one + ['asl\tr13,r13,1', 'or\tr13,r13,r25', 'lsr\tr25,r13,16', 'bmsk\tr13,r13,15'] + nz16('r13')
    elif oc == '6A':                             # ROR A
        if w8: b = one + ['bmsk\tr3,r13,7', 'asl\tr4,r25,8', 'or\tr3,r3,r4', 'and\tr25,r3,1', 'lsr\tr3,r3,1'] + alu8_merge('r3') + nz8('r3')
        else: b = one + ['asl\tr4,r25,16', 'or\tr13,r13,r4', 'and\tr25,r13,1', 'lsr\tr13,r13,1'] + nz16('r13')
    elif oc == 'EB' and md is None:              # XBA
        b = cyc(1, 12) + ['lsr\tr3,r13,8', 'bmsk\tr4,r13,7', 'asl\tr4,r4,8', 'or\tr13,r3,r4'] + nz8('r3')
    elif oc == '18' and md is None: b = one + ['mov\tr25,0']                  # CLC
    elif oc == '38' and md is None: b = one + ['mov\tr25,1']                  # SEC
    elif oc == 'B8' and md is None: b = one + ['mov\tr4,0', 'stb\tr4,[ICPU+I_OVF]']   # CLV
    elif oc in ('58', '78', 'D8', 'F8') and md is None:   # CLI SEI CLD SED (PL in memory)
        bit = 2 if oc in ('58', '78') else 3
        b = one + ['ldb\tr4,[CPU+C_PL]', 'nop', 'nop', '%s\tr4,r4,%d' % ('bclr' if oc in ('58', 'D8') else 'bset', bit), 'stb\tr4,[CPU+C_PL]']
        if oc == '58' and os.environ.get('G65_IRQFAST'): b.append('bl\tM_ne')
    elif oc == 'EA' and md is None: b = one                                   # NOP
    if b is None: return None
    return early_body(1, b) + tail(1, early=True)

def setpc():  # S9xSetPCBase(r2)
    return ['bl\tM_setpc']

def special(name):
    """everything else the asm core does itself; None = the C handler"""
    m = re.match(r'Op([0-9A-F]{2})([MX][01])?$', name)
    if not m: return None
    oc, md = m.group(1), m.group(2)
    w8 = md is not None and md[1] == '1'
    if oc in ('10', '30', '50', '70', '90', 'B0', 'D0', 'F0', '80') and md is None:
        # branches: Relative(), BranchCheck (CPU.BranchSkip: the C handler), taken: PC, +6, CPUShutdown
        cond = {'10': ('bbit1', 'r24', 15), '30': ('bbit0', 'r24', 15), 'D0': ('breq', 'r23', 0), 'F0': ('brne', 'r23', 0),
                '90': ('brne', 'r25', 0), 'B0': ('breq', 'r25', 0)}  # (branch to NOT taken)
        nt, ret, bk, wt = L(), L(), L(), L()
        if oc != '80':
            r = ['ldb	r4,[CPU+C_BRANCHSKIP]', 'ldb	r3,[r19,1]', 'ldb	r0,[r19,2]']
            if oc in ('50', '70'): r.append('ldb	r9,[ICPU+I_OVF]')
            r += ['ld	r5,[CPU+C_PCBASE]', 'brne	r4,0,FARCFB'] + cyc(2)
            if oc in ('50', '70'):
                r.append('%s	r9,0,%s' % ('brne' if oc == '50' else 'breq', nt))
            else:
                ins, reg, v = cond[oc]
                r.append('%s	%s,%d,%s' % (ins, reg, v, nt))
        else:
            r = ['ldb	r3,[r19,1]', 'ld	r5,[CPU+C_PCBASE]'] + cyc(2)
        # taken: PC = PCBase + ((PC + 2 - PCBase + s8) & 0xffff), +6, CPUShutdown (backward: idle table)
        r += ['sexb	r3,r3', 'add	r6,r19,2', 'ld	r8,[CPU+C_WAITADDR]', 'add	r6,r6,r3', 'sub	r7,r6,r5', 'bmsk	r7,r7,15',
              'mov	r11,r19', 'add	r19,r5,r7', 'ldb	r0,[r19,0]', 'add	r20,r20,6', 'brlo	r19,r11,%s' % bk, 'breq	r19,r8,%s' % wt,
              ret + ':', 'ld.as	r1,[r22,r0]', 'nop', 'nop']
        s1 = L()
        r += ['brge	r20,r21,%s' % s1, 'j.d	[r1]', 'mov	r12,r20', s1 + ':', 'b	LLevJ']
        if oc != '80':
            s2 = L()
            r += [nt + ':', 'ld.as	r1,[r22,r0]', 'add	r19,r19,2', 'nop', 'brge	r20,r21,%s' % s2, 'j.d	[r1]', 'mov	r12,r20', s2 + ':', 'b	LLev2']
        r += [bk + ':', 'bl	M_shut', 'ldb	r0,[r19,0]', 'b	' + ret, wt + ':', 'bl	M_wait', 'ldb	r0,[r19,0]', 'b	' + ret]
        return r
    if oc == '82' and md is None:      # BRL: RelativeLong, SetPCBase(ShiftedPB + OpAddress)
        return (['mov\tr11,r19'] + op16('r2') + cyc(3, 6) + ['ld\tr4,[CPU+C_PCBASE]', 'add\tr5,r19,3', 'nop', 'sub\tr5,r5,r4', 'add\tr2,r2,r5',
                'bmsk\tr2,r2,15', 'ld\tr4,[ICPU+I_SPB]', 'nop', 'nop', 'add\tr2,r2,r4'] + setpc() + tail(0, jump=True))
    if oc == '4C' and md is None:      # JMP abs
        return ['mov\tr11,r19'] + op16('r2') + cyc(3) + ['ld\tr4,[ICPU+I_SPB]', 'nop', 'nop', 'add\tr2,r2,r4'] + setpc() + tail(0, jump=True)
    if oc == '5C' and md is None:      # JML long
        return (['mov\tr11,r19'] + op24('r2') + cyc(4) + ['lsr\tr4,r2,16', 'stb\tr4,[CPU+C_PB]', 'and\tr4,r2,0xff0000', 'st\tr4,[ICPU+I_SPB]'] +
                setpc() + tail(0, jump=True))
    if oc == '6C' and md is None:      # JMP (abs): GetWord(op16) + ShiftedPB
        return (['mov\tr11,r19'] + op16('r2') + cyc(3) + rd(16, False) + ['ld\tr4,[ICPU+I_SPB]', 'nop', 'nop', 'add\tr2,r3,r4'] + setpc() + tail(0, jump=True))
    if oc in ('7C', 'FC') and md is None:   # JMP/JSR (abs,X): GetWord(ShiftedPB + ((X + op16) & 0xffff))
        r = ['mov\tr11,r19'] + op16('r2') + cyc(3) + ['add\tr2,r2,r14', 'bmsk\tr2,r2,15', 'ld\tr10,[ICPU+I_SPB]', 'nop', 'nop',
             'add\tr2,r2,r10'] + rd(16, False)
        if oc == 'FC':
            r += ['add\tr10,r3,r10', 'ld\tr4,[CPU+C_PCBASE]', 'add\tr3,r19,2', 'nop', 'sub\tr3,r3,r4', 'bmsk\tr3,r3,15'] + push16('r3') + ['mov\tr2,r10']
        else:
            r += ['add\tr2,r3,r10']
        return r + setpc() + cyc(0, 6) + tail(0, jump=True)
    if oc == 'DC' and md is None:      # JML [abs]: GetWord(op16) | GetByte(op16 + 2) << 16
        return (['mov\tr11,r19'] + op16('r2') + cyc(3) + rd(16, False) + ['mov\tr10,r3', 'add\tr2,r2,2'] + rd(8, False) +
                ['asl\tr3,r3,16', 'or\tr2,r10,r3', 'lsr\tr4,r2,16', 'stb\tr4,[CPU+C_PB]', 'and\tr4,r2,0xff0000', 'st\tr4,[ICPU+I_SPB]'] +
                setpc() + cyc(0, 12) + tail(0, jump=True))
    if oc == '20' and md is None:      # JSR abs
        return (['mov\tr11,r19'] + op16('r10') + cyc(3) + ['ld\tr4,[CPU+C_PCBASE]', 'add\tr3,r19,2', 'nop', 'sub\tr3,r3,r4', 'bmsk\tr3,r3,15'] +
                push16('r3') + ['ld\tr4,[ICPU+I_SPB]', 'nop', 'nop', 'or\tr2,r10,r4'] + setpc() + cyc(0, 6) + tail(0, jump=True))
    if oc == '22' and md is None:      # JSL long
        return (['mov\tr11,r19'] + op24('r10') + cyc(4) + ['ldb\tr3,[CPU+C_PB]', 'nop', 'nop'] + push8('r3') +
                ['ld\tr4,[CPU+C_PCBASE]', 'add\tr3,r19,3', 'nop', 'sub\tr3,r3,r4', 'bmsk\tr3,r3,15'] + push16('r3') +
                ['lsr\tr4,r10,16', 'stb\tr4,[CPU+C_PB]', 'and\tr4,r10,0xff0000', 'st\tr4,[ICPU+I_SPB]', 'mov\tr2,r10'] + setpc() + tail(0, jump=True))
    if oc == '60' and md is None:      # RTS
        return (['mov\tr11,r19'] + cyc(1) + pull16() + ['add\tr2,r3,1', 'bmsk\tr2,r2,15', 'ld\tr4,[ICPU+I_SPB]', 'nop', 'nop', 'add\tr2,r2,r4'] +
                setpc() + cyc(0, 18) + tail(0, jump=True))
    if oc == '6B' and md is None:      # RTL
        return (['mov\tr11,r19'] + cyc(1) + pull16() + ['mov\tr10,r3'] + pull8() +
                ['stb\tr3,[CPU+C_PB]', 'asl\tr4,r3,16', 'st\tr4,[ICPU+I_SPB]', 'add\tr2,r10,1', 'bmsk\tr2,r2,15', 'add\tr2,r2,r4'] +
                setpc() + cyc(0, 12) + tail(0, jump=True))
    # pushes and pulls
    if oc in ('48', 'DA', '5A'):       # PHA PHX PHY
        reg = {'48': A, 'DA': X, '5A': Y}[oc]
        return cyc(1) + (push8(reg) if w8 else push16(reg)) + cyc(0, 6) + tail(1)
    if oc == '0B' and md is None: return cyc(1) + push16(D) + cyc(0, 6) + tail(1)           # PHD
    if oc == '8B' and md is None: return cyc(1) + ['lsr\tr3,r18,16'] + push8('r3') + cyc(0, 6) + tail(1)   # PHB
    if oc == '4B' and md is None: return cyc(1) + ['ldb\tr3,[CPU+C_PB]', 'nop', 'nop'] + push8('r3') + cyc(0, 6) + tail(1)   # PHK
    if oc == '08' and md is None:      # PHP: S9xPackStatus, PushB(PL)
        return (cyc(1) + ['ldb\tr3,[CPU+C_PL]', 'ldb\tr4,[ICPU+I_OVF]', 'mov\tr5,0', 'cmp\tr23,0', 'mov.eq\tr5,2', 'and\tr3,r3,0x3c',
                'or\tr3,r3,r25', 'or\tr3,r3,r5', 'asl\tr4,r4,6', 'or\tr3,r3,r4', 'lsr\tr5,r24,8', 'and\tr5,r5,0x80', 'or\tr3,r3,r5',
                'stb\tr3,[CPU+C_PL]'] + push8('r3') + cyc(0, 6) + tail(1))
    if oc in ('68', 'FA', '7A'):       # PLA PLX PLY
        reg = {'68': A, 'FA': X, '7A': Y}[oc]
        r = cyc(1, 12) + (pull8() if w8 else pull16())
        if reg == A and w8: r += alu8_merge('r3') + nz8('r3')
        else: r += ['mov\t%s,r3' % reg] + (nz8('r3') if w8 else nz16('r3'))
        return early_body(1, r) + tail(1, early=True)
    if oc == '2B' and md is None: return early_body(1, cyc(1, 12) + pull16() + ['mov\tr17,r3'] + nz16('r3')) + tail(1, early=True)   # PLD
    if oc == 'AB' and md is None: return early_body(1, cyc(1, 12) + pull8() + ['asl\tr18,r3,16', 'st\tr18,[ICPU+I_SDB]', 'stb\tr3,[CPU+C_DB]'] + nz8('r3')) + tail(1, early=True)   # PLB
    if oc == 'F4' and md is None:      # PEA: Absolute, PushW((uint16) OpAddress)
        return op16('r3') + cyc(3) + push16('r3') + tail(3)
    if oc == 'D4' and md is None:      # PEI: DirectIndirect, PushW((uint16) OpAddress)
        return op8('r2') + cyc(2) + ['add\tr2,r2,r17', 'bmsk\tr2,r2,15'] + rd(16, True) + push16('r3') + tail(2)
    if oc == '62' and md is None:      # PER: RelativeLong, PushW
        return (op16('r2') + cyc(3, 6) + ['ld\tr4,[CPU+C_PCBASE]', 'add\tr5,r19,3', 'nop', 'sub\tr5,r5,r4', 'add\tr3,r2,r5', 'bmsk\tr3,r3,15'] +
                push16('r3') + tail(3))
    if oc in ('C2', 'E2') and md is None:   # REP SEP (raw flag bytes as in C), S9xFixCycles
        r = op8('r3') + cyc(2, 6) + ['ldb\tr4,[CPU+C_PL]', 'nop', 'nop']
        if oc == 'C2':
            k1, k2 = L(), L()
            r += ['bic\tr4,r4,r3', 'stb\tr4,[CPU+C_PL]', 'btst\tr3,0', 'mov.ne\tr25,0', 'asl\tr5,r3,8', 'bic\tr24,r24,r5',
                  'bbit0\tr3,6,%s' % k1, 'mov\tr5,0', 'stb\tr5,[ICPU+I_OVF]', k1 + ':',
                  'bbit0\tr3,1,%s' % k2, 'cmp\tr23,0xff', 'mov.hi\tr23,1', 'or\tr23,r23,2', k2 + ':']
        else:
            k1 = L()
            r += ['or\tr4,r4,r3', 'stb\tr4,[CPU+C_PL]', 'and\tr5,r3,1', 'or\tr25,r25,r5', 'asl\tr5,r3,8', 'or\tr24,r24,r5',
                  'bbit0\tr3,6,%s' % k1, 'mov\tr5,1', 'stb\tr5,[ICPU+I_OVF]', k1 + ':', 'btst\tr3,1', 'mov.ne\tr23,0']
        r += ['bl\tM_mode']
        return r + tail(2)
    if oc == '28' and md is None:      # PLP: PullB(PL), S9xUnpackStatus, S9xFixCycles
        k1 = L()
        return (cyc(1, 12) + pull8() + ['stb\tr3,[CPU+C_PL]', 'mov\tr4,r3', 'and\tr25,r3,1', 'lsr\tr5,r3,6', 'and\tr5,r5,1', 'stb\tr5,[ICPU+I_OVF]',
                'and\tr24,r3,0x80', 'asl\tr24,r24,8', 'xor\tr23,r3,2', 'lsr\tr23,r23,1', 'and\tr23,r23,1', 'bl\tM_mode'] + tail(1))
    if oc == 'CB' and md is None and not os.environ.get('G65_NOWAI'):      # WAI: IRQActive: 2 cycles; else wait (PC stays), Cycles = NextEvent, the APU runs
        k = L()
        wait = ([k + ':', 'mov\tr4,1', 'stb\tr4,[CPU+C_WAITINT]', 'ld\tr20,[CPU+C_NEXTEVENT]', 'bl\tM_apu2'] + tail(0)
                if not os.environ.get('G65_WAITC') else [k + ':', 'sub\tr20,r20,r27', 'b\tLLcfb'])
        # IRQActive: LL_SPEEDHACKS: WAI followed by BRA back to it (80 FD) goes to the next event at once
        k2 = L()
        return (['ldb\tr4,[CPU+C_IRQACTIVE]', 'ldb\tr5,[r19,1]', 'ldb\tr6,[r19,2]', 'ld\tr7,[CPU+C_NEXTEVENT]'] + cyc(1) +
                ['breq\tr4,0,%s' % k, 'brne\tr5,0x80,%s' % k2, 'brne\tr6,0xfd,%s' % k2, 'brge\tr20,r7,%s' % k2,
                 'mov\tr20,r7', 'bl\tM_apu2', 'b\t%s_1' % k2, k2 + ':'] + cyc(0, 12) + [k2 + '_1:'] + tail(1) + wait)
    if oc in ('54', '44'):             # MVN MVP
        inc = 'add' if oc == '54' else 'sub'
        top = 7 if w8 else 15
        k = L()
        return (cyc(3, 12) + ['ldb\tr4,[r19,1]', 'ldb\tr2,[r19,2]', 'nop', 'stb\tr4,[CPU+C_DB]', 'asl\tr18,r4,16', 'st\tr18,[ICPU+I_SDB]',
                'asl\tr2,r2,16', 'add\tr2,r2,r14', 'bl\tM_rd8', 'add\tr2,r18,r15', 'bl\tM_wr8',
                '%s\tr14,r14,1' % inc, '%s\tr15,r15,1' % inc, 'bmsk\tr14,r14,%d' % top, 'bmsk\tr15,r15,%d' % top,
                'sub\tr13,r13,1', 'bmsk\tr13,r13,15', 'mov\tr11,r19', 'add\tr19,r19,3', 'brne\tr13,0xffff,%s' % k, 'b\t%s_done' % name.replace('Op', 'H'),
                k + ':', 'mov\tr19,r11', name.replace('Op', 'H') + '_done:'] + tail(0, jump=True))
    return None

def gen_handler(name, funcs, xmode):
    """xmode: the X flag of the table (for Op81's extra cycle)"""
    b = funcs.get(name)
    if b is None: return None
    if len(b) == 2 and all(re.match(r'\w+\(\);$', x) for x in b):
        return two_call(name, b[0][:-3], b[1][:-3])
    if name in ('Op81M0', 'Op81M1'):   # STA (dp,X), one cycle more with 8-bit index registers
        code, n, fast = ea('DirectIndexedIndirect', 8)
        body, _ = oper('STA16' if name == 'Op81M0' else 'STA8', fast)
        return code + body + (cyc(0, 6) if xmode else []) + tail(n)
    for f in (imm, implied, special):
        r = f(name)
        if r is not None: return r
    return None

# ---- fixed code: entry/exit, between instructions, C fallback, memory helpers
FIXED = r'''
	.text
	.align 4
; LL65Run(): runs the 65816 until LL65Between says the C loop would leave (SCAN_KEYS_FLAG)
	.global LL65Run
LL65Run:
	sub	sp,sp,64
	st	blink,[sp,0]
	st	r13,[sp,4]
	st	r14,[sp,8]
	st	r15,[sp,12]
	st	r16,[sp,16]
	st	r17,[sp,20]
	st	r18,[sp,24]
	st	r19,[sp,28]
	st	r20,[sp,32]
	st	r21,[sp,36]
	st	r22,[sp,40]
	st	r23,[sp,44]
	st	r24,[sp,48]
	st	r25,[sp,52]
	st	r27,[sp,56]
	st	sp,[LL65SP]
	mov	r0,0
	bl	LL65Between		; the C loop's flag checks before the first instruction
	breq	r0,1,LLexit
	bl	LL65Load
LLgo:	ldb	r0,[r19,0]
	nop
	nop
	ld.as	r1,[r22,r0]
	nop
	nop
	j.d	[r1]
	mov	r12,r20
LLexit:
	ld	sp,[LL65SP]
	ld	blink,[sp,0]
	ld	r13,[sp,4]
	ld	r14,[sp,8]
	ld	r15,[sp,12]
	ld	r16,[sp,16]
	ld	r17,[sp,20]
	ld	r18,[sp,24]
	ld	r19,[sp,28]
	ld	r20,[sp,32]
	ld	r21,[sp,36]
	ld	r22,[sp,40]
	ld	r23,[sp,44]
	ld	r24,[sp,48]
	ld	r25,[sp,52]
	ld	r27,[sp,56]
	j.d	[blink]
	add	sp,sp,64

; shared dispatch tails: PC += n, next opcode, event check
LLtJ:	ldb	r0,[r19,0]
	nop
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLevJ
	j.d	[r1]
	mov	r12,r20
LLt0:	ldb	r0,[r19,0]
	nop
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLev0
	j.d	[r1]
	mov	r12,r20
LLt1:	ldb	r0,[r19,1]
	add	r19,r19,1
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLev1
	j.d	[r1]
	mov	r12,r20
LLt2:	ldb	r0,[r19,2]
	add	r19,r19,2
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLev2
	j.d	[r1]
	mov	r12,r20
LLt3:	ldb	r0,[r19,3]
	add	r19,r19,3
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLev3
	j.d	[r1]
	mov	r12,r20
LLt4:	ldb	r0,[r19,4]
	add	r19,r19,4
	nop
	ld.as	r1,[r22,r0]
	nop
	brge	r20,r21,LLev4
	j.d	[r1]
	mov	r12,r20

; end of an instruction with CPU.Cycles >= r21: PCAtOpcodeStart, LLOpCycles of that instruction
LLev0:	st	r19,[CPU+C_PCOP]
	b	LLev
LLev1:	sub	r2,r19,1
	st	r2,[CPU+C_PCOP]
	b	LLev
LLev2:	sub	r2,r19,2
	st	r2,[CPU+C_PCOP]
	b	LLev
LLev3:	sub	r2,r19,3
	st	r2,[CPU+C_PCOP]
	b	LLev
LLev4:	sub	r2,r19,4
	st	r2,[CPU+C_PCOP]
	b	LLev
LLevJ:	st	r11,[CPU+C_PCOP]
LLev:	st	r12,[LLOpCycles]
EVFAST
	; no line event due, only an IRQ pending with nothing to do this time (counting down,
	; or masked by the I flag): what the C loop's flag check does, without leaving asm
	ld	r4,[CPU+C_NEXTEVENT]
	ld	r5,[CPU+C_FLAGS]
	ld	r6,[CPU+C_IRQCOUNT]
	brge	r20,r4,LLevc
	brne	r5,0x800,LLevc		; IRQ_PENDING_FLAG alone
	breq	r6,0,1f
	sub	r6,r6,1
	st	r6,[CPU+C_IRQCOUNT]
	b	LLgo2
1:	ldb	r4,[CPU+C_WAITINT]
	ldb	r5,[CPU+C_IRQACTIVE]
	ldb	r6,[Settings+S_DISABLEIRQ]
	brne	r4,0,LLevc
	breq	r5,0,LLevc
	brne	r6,0,LLevc
	ldb	r4,[CPU+C_PL]
IRQFAST3
LLgo2:	ldb	r0,[r19,0]
	nop
	nop
	ld.as	r1,[r22,r0]
	nop
	nop
	j.d	[r1]
	mov	r12,r20
LLevc:
	bl	LL65Spill
	mov	r0,1
	bl	LL65Between
	breq	r0,0,1f
	breq	r0,1,LLexit2
	bl	LL65Load
	b	LLgo
1:	ld	r19,[CPU+C_PC]		; (S9xSetIRQ after WAI moves it)
	ld	r20,[CPU+C_CYCLES]
	bl	M_ne
	b	LLgo
LLexit2:
	b	LLexit

; an opcode for the C handler (r19 = its address, nothing done yet)
LLcfb:
	st	r19,[CPU+C_PCOP]
	st	r12,[LLOpCycles]
	add	r20,r20,r27		; the C loop's opcode fetch
	add	r19,r19,1
	bl	LL65Spill
	ldb	r0,[r19,-1]
	bl	LL65Op
	bl	LL65Load
	ld	r12,[LLOpCycles]	; (C clobbers r12: LLev stores it as the opcode's start cycles)
	brge	r20,r21,LLev		; (PCAtOpcodeStart, LLOpCycles are set)
	b	LLgo

; the registers into the C structs (CPU.PC = r19); clobbers r4-r6
LL65Spill:
	stw	r13,[CPU+C_A]
	stw	r14,[CPU+C_X]
	stw	r15,[CPU+C_Y]
	stw	r16,[CPU+C_S]
	stw	r17,[CPU+C_D]
	lsr	r4,r18,16
	stb	r4,[CPU+C_DB]
	st	r18,[ICPU+I_SDB]
	st	r19,[CPU+C_PC]
	st	r20,[CPU+C_CYCLES]
	stb	r25,[ICPU+I_CARRY]
	lsr	r5,r24,8
	stb	r5,[ICPU+I_NEG]
	mov	r6,r23
	cmp	r23,0xff
	mov.hi	r6,1
	stb	r6,[ICPU+I_ZERO]
	j	[blink]

; everything back from the C structs (after C ran instructions or interrupts)
LL65Load:
	ldw	r13,[CPU+C_A]
	ldw	r14,[CPU+C_X]
	ldw	r15,[CPU+C_Y]
	ldw	r16,[CPU+C_S]
	ldw	r17,[CPU+C_D]
	ldb	r18,[CPU+C_DB]
	ld	r19,[CPU+C_PC]
	ld	r20,[CPU+C_CYCLES]
	ld	r27,[CPU+C_MEMSPEED]
	ldb	r23,[ICPU+I_ZERO]
	ldb	r24,[ICPU+I_NEG]
	ldb	r25,[ICPU+I_CARRY]
	asl	r18,r18,16
	asl	r24,r24,8
	mov	r12,r20
	; fall into M_mode, then the next-event register
	mov	r5,blink
	bl	M_mode
	mov	blink,r5
M_ne:	ld	r21,[CPU+C_NEXTEVENT]
	ld	r4,[CPU+C_FLAGS]
	nop
	nop
	tst	r4,r4
	mov.ne	r21,0x80000000
	j	[blink]

; the dispatch table for PL/PH (S9xFixCycles); X flag set: XH = YH = 0. Clobbers r3, r4.
M_mode:
	ldb	r3,[CPU+C_PL+1]
	ldb	r4,[CPU+C_PL]
	nop
	bbit1	r3,0,1f
	bbit0	r4,4,2f
	bmsk	r14,r14,7
	bmsk	r15,r15,7
2:	lsr	r3,r4,4
	and	r3,r3,3
	ld.as	r22,[LL65Tabs,r3]
MMODERET
1:	ld	r22,[LL65Tabs+16]
	j	[blink]

; ---- memory: generic accesses through LL65Map (getset.h). r2 = address (any 32 bits).
; Clobber r3 (result), r4, r5; the C paths keep everything else.
M_rd8:
	lsr	r4,r2,8
	and	r4,r4,0xfff0
	ld	r5,[r4,LL65Map]
	ld	r3,[r4,LL65Map+8]
	ld	r4,[r4,LL65Map+12]
	brlo	r5,MAPLAST,M_rd8_c
	add	r20,r20,r3
	bmsk	r3,r2,15
	ldb	r3,[r5,r3]
	j.d	[blink]
	st	r19,[r4,0]		; RAM: WaitAddress = PCAtOpcodeStart
M_rd8_c:
	mov	r4,LLGetByte
	b	M_ccall

M_rd16:
	bmsk	r3,r2,12
	breq	r3,0x1fff,M_rd16_s	; S9xGetWord: two bytes at $xx1FFF
	lsr	r4,r2,8
	and	r4,r4,0xfff0
	ld	r5,[r4,LL65Map]
	ld	r3,[r4,LL65Map+8]
	ld	r4,[r4,LL65Map+12]
	brlo	r5,MAPLAST,M_rd16_c
	add1	r20,r20,r3
	bmsk	r3,r2,15
	add	r5,r5,r3
	ldb	r3,[r5,0]
	ldb	r5,[r5,1]
	st	r19,[r4,0]
	nop
	asl	r5,r5,8
	j.d	[blink]
	or	r3,r3,r5
M_rd16_c:
	mov	r4,LLGetWord
	b	M_ccall
M_rd16_s:
	st.a	blink,[sp,-8]
	st	r2,[sp,4]
	bl	M_rd8
	ld	r2,[sp,4]
	st	r3,[sp,4]
	add	r2,r2,1
	bl	M_rd8
	ld	r4,[sp,4]
	sub	r2,r2,1
	asl	r3,r3,8
	ld.ab	blink,[sp,8]
	j.d	[blink]
	or	r3,r3,r4

; fast-first helpers for bank 0 16-bit addresses (direct page, stack): WRAM below $2000
; (speed 8, RAM: WaitAddress), else the generic ones. Reads keep r0-r2; writes clobber r0, r1.
M_frd8:
	brhs	r2,0x2000,M_rd8
	ld	r4,[LL65Ram]
	add	r20,r20,8
	st	r19,[CPU+C_WAITADDR]
	j.d	[blink]
	ldb	r3,[r4,r2]
M_frd16:
	brhs	r2,0x1fff,M_rd16
	ld	r4,[LL65Ram]
	add	r20,r20,16
	st	r19,[CPU+C_WAITADDR]
	add	r4,r4,r2
	ldb	r3,[r4,0]
	ldb	r5,[r4,1]
	nop
	nop
	asl	r5,r5,8
	j.d	[blink]
	or	r3,r3,r5
M_fwr8:
	brhs	r2,0x2000,M_wr8
	ld	r4,[LL65Ram]
	ld	r1,[LLWriteCount]
	mov	r0,0
	add	r4,r4,r2
	ldb	r5,[r4,0]
	st	r0,[CPU+C_WAITADDR]
	stb	r3,[r4,0]
	add	r20,r20,8
	xor	r3,r3,r5
	extb.f	0,r3
	add.ne	r1,r1,1
	st	r1,[LLWriteCount]
	j	[blink]
M_fwr16:
	brhs	r2,0x1fff,M_wr16
	ld	r4,[LL65Ram]
	ld	r1,[LLWriteCount]
	mov	r0,0
	add	r4,r4,r2
	ldb	r5,[r4,0]
	ldb	r0,[r4,1]
	stb	r3,[r4,0]
	xor	r5,r5,r3
	lsr	r3,r3,8
	stb	r3,[r4,1]
	xor	r0,r0,r3
	extb	r5,r5
	or.f	0,r0,r5
	mov	r0,0
	st	r0,[CPU+C_WAITADDR]
	add	r20,r20,16
	add.ne	r1,r1,1
	st	r1,[LLWriteCount]
	j	[blink]

; writes: r3 = data. WaitAddress = NULL, LLWriteCount += changed (S9xSetWord: one count, no block split)
M_wr8:
	lsr	r4,r2,8
	and	r4,r4,0xfff0
	ld	r5,[r4,LL65Map+4]
	ld	r4,[r4,LL65Map+8]
	nop
	brlo	r5,MAPLAST,M_wr8_c
	add	r20,r20,r4
	bmsk	r4,r2,15
	add	r5,r5,r4
	ldb	r4,[r5,0]
	stb	r3,[r5,0]
	mov	r5,0
	st	r5,[CPU+C_WAITADDR]
	extb	r5,r3
	breq	r4,r5,1f
	ld	r4,[LLWriteCount]
	nop
	nop
	add	r4,r4,1
	st	r4,[LLWriteCount]
1:	j	[blink]
M_wr8_c:
#ifdef LL_VRAMB
	cmp	r5,0
	beq	M_wr8_v
#endif
M_wr8_cc:
	mov	r4,LLSetByte
	b	M_ccallw

M_wr16:
	lsr	r4,r2,8
	and	r4,r4,0xfff0
	ld	r5,[r4,LL65Map+4]
	ld	r4,[r4,LL65Map+8]
	nop
	brlo	r5,MAPLAST,M_wr16_c
	add1	r20,r20,r4
	st.a	r6,[sp,-8]
	st	r7,[sp,4]
	bmsk	r4,r2,15
	add	r6,r5,r4		; SetAddress + (Address & 0xffff)
	add	r4,r4,1
	bmsk	r4,r4,15
	add	r5,r5,r4		; SetAddress + ((Address + 1) & 0xffff)
	ldb	r4,[r6,0]
	ldb	r7,[r5,0]
	stb	r3,[r6,0]
	lsr	r6,r3,8
	stb	r6,[r5,0]
	xor	r7,r7,r6
	extb	r6,r3
	xor	r4,r4,r6
	or	r7,r7,r4
	extb.f	0,r7
	mov	r5,0
	st	r5,[CPU+C_WAITADDR]
	ld	r6,[LLWriteCount]
	nop
	nop
	add.ne	r6,r6,1
	st	r6,[LLWriteCount]
	ld	r7,[sp,4]
	j.d	[blink]
	ld.ab	r6,[sp,8]
M_wr16_c:
#ifdef LL_VRAMW
; a word to 2118 (VRAM data, r5 = MAP_PPU = 0): LLVramW without the register spill (S9xSetWord's
; PPU path reads no 65816 register); returns the cycles it added to CPU.Cycles
	brne	r5,0,9f
	bmsk	r4,r2,15
	brne	r4,0x2118,9f
	sub	sp,sp,48
	st	blink,[sp,0]
	st	r0,[sp,4]
	st	r1,[sp,8]
	st	r2,[sp,12]
	st	r6,[sp,16]
	st	r7,[sp,20]
	st	r8,[sp,24]
	st	r9,[sp,28]
	st	r10,[sp,32]
	st	r11,[sp,36]
	st	r12,[sp,40]
	mov	r0,r3
	bl.d	LLVramW
	mov	r1,r2
	add	r20,r20,r0
	ld	blink,[sp,0]
	ld	r0,[sp,4]
	ld	r1,[sp,8]
	ld	r2,[sp,12]
	ld	r6,[sp,16]
	ld	r7,[sp,20]
	ld	r8,[sp,24]
	ld	r9,[sp,28]
	ld	r10,[sp,32]
	ld	r11,[sp,36]
	ld	r12,[sp,40]
	j.d	[blink]
	add	sp,sp,48
9:
#endif
	mov	r4,LLSetWord
	b	M_ccallw

; call C r4: reads (r2) -> r3, writes (r3, r2). Spills the registers (the APU port code
; reads them), keeps r0-r2, r6-r12; then CPU.Cycles / NextEvent / Flags back.
#ifdef LL_VRAMB
M_wr8_v:
; M7: a byte to 2118/2119 (VRAM data, r5 = MAP_PPU = 0): LLVramB without the register spill (S9xSetByte's
; PPU path for these reads no 65816 register); returns the cycles it added to CPU.Cycles
	bmsk	r4,r2,15
	bclr	r4,r4,0
	cmp	r4,0x2118
	bne	M_wr8_cc
	sub	sp,sp,48
	st	blink,[sp,0]
	st	r0,[sp,4]
	st	r1,[sp,8]
	st	r2,[sp,12]
	st	r6,[sp,16]
	st	r7,[sp,20]
	st	r8,[sp,24]
	st	r9,[sp,28]
	st	r10,[sp,32]
	st	r11,[sp,36]
	st	r12,[sp,40]
	mov	r0,r3
	bl.d	LLVramB
	mov	r1,r2
	add	r20,r20,r0
	ld	blink,[sp,0]
	ld	r0,[sp,4]
	ld	r1,[sp,8]
	ld	r2,[sp,12]
	ld	r6,[sp,16]
	ld	r7,[sp,20]
	ld	r8,[sp,24]
	ld	r9,[sp,28]
	ld	r10,[sp,32]
	ld	r11,[sp,36]
	ld	r12,[sp,40]
	j.d	[blink]
	add	sp,sp,48
#endif
M_ccallw:
	sub	sp,sp,56
	st	r3,[sp,48]
	b.d	1f
	mov	r3,1
M_ccall:
	sub	sp,sp,56
	mov	r3,0
1:	st	blink,[sp,0]
	st	r0,[sp,4]
	st	r1,[sp,8]
	st	r2,[sp,12]
	st	r6,[sp,16]
	st	r7,[sp,20]
	st	r8,[sp,24]
	st	r9,[sp,28]
	st	r10,[sp,32]
	st	r11,[sp,36]
	st	r12,[sp,40]
	st	r4,[sp,44]
	st	r3,[sp,52]
	st	r19,[CPU+C_PCOP]
	st	r12,[LLOpCycles]
	bl	LL65Spill
	ld	r3,[sp,52]
	ld	r4,[sp,44]
	breq.d	r3,0,2f
	mov	r0,r2
	ld	r0,[sp,48]
	mov	r1,r2
2:	jl	[r4]
	mov	r3,r0
	ld	r20,[CPU+C_CYCLES]
	bl	M_ne
	ld	blink,[sp,0]
	ld	r0,[sp,4]
	ld	r1,[sp,8]
	ld	r2,[sp,12]
	ld	r6,[sp,16]
	ld	r7,[sp,20]
	ld	r8,[sp,24]
	ld	r9,[sp,28]
	ld	r10,[sp,32]
	ld	r11,[sp,36]
	ld	r12,[sp,40]
	j.d	[blink]
	add	sp,sp,56

; S9xSetPCBase(r2): r19, r27, CPU.PCBase/MemSpeed/MemSpeedx2
M_setpc:
	lsr	r4,r2,8
	and	r4,r4,0xfff0
	ld	r5,[r4,LL65Map]
	ld	r3,[r4,LL65Map+8]
	nop
	brlo	r5,MAPLAST,M_setpc_c
	st	r5,[CPU+C_PCBASE]
	mov	r27,r3
	st	r3,[CPU+C_MEMSPEED]
	asl	r3,r3,1
	st	r3,[CPU+C_MEMSPEED2]
	bmsk	r4,r2,15
	j.d	[blink]
	add	r19,r5,r4
M_setpc_c:
	st.a	blink,[sp,-4]
	mov	r4,LLSetPCBase
	bl	M_ccall
	ld	r19,[CPU+C_PC]
	ld	r27,[CPU+C_MEMSPEED]
	ld.ab	blink,[sp,4]
	j	[blink]

; asm_APU_EXECUTE2 (WAI): the sound CPU up to CPU.NextEvent
M_apu2:
	ld	r4,[CPU+C_APUEXEC]
#ifdef LL_SPCDEFER
	ld	r5,[LLSpcN]
	nop
	breq	r4,1,2f
	breq	r5,0,1f		; SPC shut down, nothing queued: LL65Apu2 would do nothing
2:
#else
	nop
	nop
	brne	r4,1,1f
#endif
	sub	sp,sp,56
	st	blink,[sp,0]
	st	r0,[sp,4]
	st	r1,[sp,8]
	st	r2,[sp,12]
	st	r6,[sp,16]
	st	r7,[sp,20]
	st	r8,[sp,24]
	st	r9,[sp,28]
	st	r10,[sp,32]
	st	r11,[sp,36]
	st	r12,[sp,40]
	st	r3,[sp,44]
	st	r20,[CPU+C_CYCLES]
	bl	LL65Apu2
	ld	blink,[sp,0]
	ld	r0,[sp,4]
	ld	r1,[sp,8]
	ld	r2,[sp,12]
	ld	r6,[sp,16]
	ld	r7,[sp,20]
	ld	r8,[sp,24]
	ld	r9,[sp,28]
	ld	r10,[sp,32]
	ld	r11,[sp,36]
	ld	r12,[sp,40]
	ld	r3,[sp,44]
	add	sp,sp,56
1:	j	[blink]

;; CPUShutdown after a taken branch: r19 = target, r11 = the branch (PCAtOpcodeStart).
; Clobbers r0-r10. Backward: the idle table (LLIdle, cpuops.c), with LLIdleCheck's register
; comparison done here; only a real idle loop (or CPU.WaitAddress == target) calls C.
M_shut:
	cmp	r19,r11
	bhs	M_wait
	lsr	r4,r19,1
	and	r4,r4,15
	asl	r4,r4,5
	add	r4,r4,LLIdle
	ld	r5,[r4,0]
	ld	r3,[LLWriteCount]
	ld	r6,[r4,4]
	brne	r5,r19,9f
	brne	r6,r3,9f
	; same target, no write since: LLIdleCheck (ax, yd, sb, f as cpuops.c packs them)
CIDLE
	asl	r7,r14,16
	or	r7,r7,r13		; ax
	asl	r9,r17,16
	or	r9,r9,r15		; yd
	ldb	r10,[CPU+C_PL]
	ldb	r8,[ICPU+I_OVF]
	lsr	r5,r24,8
	asl	r10,r10,24
	or	r10,r10,r18
	or	r10,r10,r16		; sb
	mov	r6,r23
	cmp	r23,0xff
	mov.hi	r6,1
	asl	r8,r8,24
	asl	r5,r5,16
	asl	r6,r6,8
	or	r5,r5,r8
	or	r5,r5,r6
	or	r5,r5,r25		; f
	ld	r6,[r4,24]
	ld	r8,[r4,8]
	ld	r3,[r4,12]
	breq	r6,0,8f
	brne	r7,r8,8f
	brne	r9,r3,8f
	ld	r8,[r4,16]
	ld	r3,[r4,20]
	ld	r6,[CPU+C_NEXTEVENT]
	brne	r10,r8,8f
	brne	r5,r3,8f
	ld	r8,[CPU+C_FLAGS]
	brge	r20,r6,8f
IRQSKIPBR
	mov	r3,LL65Idle		; an idle loop: C sleeps it (then the wait check)
	b	M_shutc
8:
M_shut8:	st	r7,[r4,8]
	st	r9,[r4,12]
	st	r10,[r4,16]
	st	r5,[r4,20]
	mov	r3,1
	ld	r5,[LLIoReads]
	st	r3,[r4,24]
	b.d	M_wait
	st	r5,[r4,28]
IRQSKIP9:	st	r19,[r4,0]
	st	r3,[r4,4]
	ld	r5,[LLIoReads]
	mov	r3,0
	st	r3,[r4,24]
	st	r5,[r4,28]
M_wait:
	ld	r5,[CPU+C_WAITADDR]
	nop
	nop
	brne	r5,r19,1f
	mov	r3,LLAsmWait
	b	M_shutc
1:	j	[blink]
IRQSKIPK
M_shutc:			; C r3(r4) with the state spilled (CPU.PC = target)
	sub	sp,sp,24
	st	blink,[sp,0]
	st	r11,[sp,4]
	st	r12,[sp,8]
	st	r3,[sp,12]
	st	r4,[sp,16]
	st	r11,[CPU+C_PCOP]
	st	r12,[LLOpCycles]
	bl	LL65Spill
	ld	r3,[sp,12]
	ld	r0,[sp,16]
	jl	[r3]
	ld	r20,[CPU+C_CYCLES]
	bl	M_ne
	ld	blink,[sp,0]
	ld	r11,[sp,4]
	ld	r12,[sp,8]
	j.d	[blink]
	add	sp,sp,24

	.section .bss
	.align 4
LL65SP:	.space	4
	.text
'''

def farfix(code):
    """BRcc reaches +-256 bytes: a compare-branch to a label more than ~30 lines on becomes cmp + Bcc"""
    pos = {l[:-1]: i for i, l in enumerate(code) if l.endswith(':')}
    out = []
    for i, l in enumerate(code):
        m = re.match(r'(br(?:hs|lo|eq|ne|lt|ge))\t(\w+),(\w+),([.\w]+)$', l)
        k = re.match(r'bbit([01])\t(\w+),(\d+),([.\w]+)$', l)
        if m and m.group(4) in pos and abs(pos[m.group(4)] - i) > 20:
            out += ['cmp\t%s,%s' % (m.group(2), m.group(3)), 'b%s\t%s' % (m.group(1)[2:], m.group(4))]
        elif k and k.group(4) in pos and abs(pos[k.group(4)] - i) > 20:
            out += ['btst\t%s,%s' % (k.group(2), k.group(3)), ('bne\t' if k.group(1) == '1' else 'beq\t') + k.group(4)]
        else:
            out.append(l)
    return out

def main():
    funcs, tabs = parseops.parse()
    body = []
    labels = {}
    tabout = {}
    missing = set()
    for mode in ('M1X1', 'M1X0', 'M0X1', 'M0X0'):
        xm = mode.endswith('X1')
        row = []
        for n in tabs[mode]:
            key = n + ('_x1' if (n.startswith('Op81') and xm) else ('_x0' if n.startswith('Op81') else ''))
            if key not in labels:
                del DEFER[:]
                code = gen_handler(n, funcs, xm)
                if code is not None:
                    code = code + DEFER
                    if any('FARCFB' in l for l in code):
                        cl = 'H_' + key[2:] + '_cfb'
                        code = [l.replace('FARCFB', cl) for l in code] + [cl + ':', 'b\tLLcfb']
                    code = farfix(code)
                if code is None:
                    labels[key] = 'LLcfb'
                    missing.add(n)
                else:
                    lb = 'H_' + key[2:]
                    labels[key] = lb
                    body.append('\t.align 2\n%s:' % lb)
                    for l in code:
                        body.append(l if l.endswith(':') else '\t' + l)
            row.append(labels[key])
        tabout[mode] = row
    fx = FIXED.replace('CIDLE\n', '\tmov\tr3,LL65Idle\n\tb\tM_shutc\n' if os.environ.get('G65_CIDLE') else '')
    # C2 (-DLL_IRQSKIP via G65_IRQSKIP): a pure loop with only a masked IRQ pending: LL65IrqSkip (cpuops.c) jumps
    # whole iterations up to the next event (exact: each iteration leaves the same state); the idle entry is stored first
    if os.environ.get('G65_IRQSKIP'):
        fx = fx.replace('IRQSKIPBR\n', '\tand.f\tr8,r8,0x880\t\t; IRQ_PENDING_FLAG | NMI_FLAG\n\tbne\t7f\n').replace('IRQSKIP9:', '9:').replace('IRQSKIPK\n', '\t.pushsection .text.llirqskip,"ax"\n7:\tcmp\tr8,0x800\n\tbne\tM_shut8\n\tld\tr8,[CPU+C_IRQCOUNT]\n\tldb\tr6,[CPU+C_PL]\n\tcmp\tr8,0\n\tbne\tM_shut8\n\tbtst\tr6,2\n\tbeq\tM_shut8\n\tst\tr7,[r4,8]\n\tst\tr9,[r4,12]\n\tst\tr10,[r4,16]\n\tst\tr5,[r4,20]\n\tmov\tr3,1\n\tld\tr5,[LLIoReads]\n\tst\tr3,[r4,24]\n\tst\tr5,[r4,28]\n\tsub\tsp,sp,4\n\tst\tblink,[sp,0]\n\tmov\tr3,LL65IrqSkip\n\tbl\tM_shutk\n\tld\tblink,[sp,0]\n\tj.d\t[blink]\t\t; (LL65IrqSkip did the M_wait part)\n\tadd\tsp,sp,4\nM_shutk:\t\t\t; M_shutc, but r12 = LLOpCycles after the call (LL65IrqSkip moves it)\n\tsub\tsp,sp,24\n\tst\tblink,[sp,0]\n\tst\tr11,[sp,4]\n\tst\tr12,[sp,8]\n\tst\tr3,[sp,12]\n\tst\tr4,[sp,16]\n\tst\tr11,[CPU+C_PCOP]\n\tst\tr12,[LLOpCycles]\n\tbl\tLL65Spill\n\tld\tr3,[sp,12]\n\tld\tr0,[sp,16]\n\tjl\t[r3]\n\tld\tr20,[CPU+C_CYCLES]\n\tbl\tM_ne\n\tld\tblink,[sp,0]\n\tld\tr11,[sp,4]\n\tld\tr12,[LLOpCycles]\n\tj.d\t[blink]\n\tadd\tsp,sp,24\n\t.popsection\n')
    else:
        fx = fx.replace('IRQSKIPBR\n', '\tand\tr8,r8,0x880\t\t; IRQ_PENDING_FLAG | NMI_FLAG\n\tbrne\tr8,0,8f\n').replace('IRQSKIP9:', '9:').replace('IRQSKIPK\n', '')
    # C3 (-DLL_IRQFAST via G65_IRQFAST): with only a masked IRQ pending (count 0), r21 = NextEvent instead of an exit after
    # every instruction; every C call ends in M_ne (back to per-instruction checks), CLI/REP/PLP call M_ne (I cleared)
    # C3: REP/SEP/PLP (M_mode, r4 = PL) with the I flag clear return through M_ne; b instead of j: the same size
    fx = fx.replace('MMODERET\n', ('\tb\tM_modeq\n\t.pushsection .text.llirqf,"ax"\nM_modeq:\tbbit1\tr4,2,M_modeqj\n\tb\tM_ne\n'
                    'M_modeqj:\tj\t[blink]\n\t.popsection\n') if os.environ.get('G65_IRQFAST') else '\tj\t[blink]\n')
    # C3: the I-flag test without the load-delay nops, then a jump to LLirqf (own section: r21 = NextEvent): same size
    fx = fx.replace('IRQFAST3\n', ('\tbbit0\tr4,2,LLevc\t\t; I set: stays pending\n\tb\tLLirqf\n\tnop\n'
                    '\t.pushsection .text.llirqf,"ax"\nLLirqf:\tld\tr21,[CPU+C_NEXTEVENT]\n\tb\tLLgo2\n\t.popsection\n')
                    if os.environ.get('G65_IRQFAST') else '\tnop\n\tnop\n\tbbit0\tr4,2,LLevc\t\t; I set: stays pending\n')
    fx = fx.replace('EVFAST\n', '\tb\tLLevc\n' if os.environ.get('G65_NOEVFAST') else '')
    o = ['; ll65.S: generated by kern/gen65.py (do not edit)\n', open(os.environ.get('LLOFFS65') or os.path.join(H, '..', 's9x', 'src', 'lloffs65.inc')).read(), fx]
    o += [l + '\n' for l in body]
    o.append('\n\t.section .rodata\n\t.align 4\n')
    for mode, nm in (('M0X0', 'LL65TabM0X0'), ('M0X1', 'LL65TabM0X1'), ('M1X0', 'LL65TabM1X0'), ('M1X1', 'LL65TabM1X1')):
        o.append('\t.global %s\n%s:\n' % (nm, nm))
        o += ['\t.word\t%s\n' % x for x in tabout[mode]]
    o.append('\t.global LL65TabE\nLL65TabE:\n' + '\t.word\tLLcfb\n' * 256)
    dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(H, '..', 's9x', 'src', 'll65.S')
    open(dst, 'w').write(''.join(o))
    print('ll65.S: %d handlers, C: %s' % (len(set(labels.values())) - 1, ' '.join(sorted(missing))))

main()
