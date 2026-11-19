# mkz80x.py: writes z80/z80x.S, the -DZ80ASM Z80 fast path. cz80.c (patched by
# mkz80.py) calls z80x_run(CPU, PC) at the top of each instruction; it runs the
# opcodes it knows (the common ones; the same results, cycle charges, R and
# memory accesses in the same order as Cz80, see ../picodrive/cpu/cz80/*.c)
# until the cycle count is <= 0 or an opcode it leaves to Cz80 comes up
# (PC/R/cycles restored to that opcode's start), then stores the registers back
# and returns PC. Left to Cz80: DAA, HALT, DI, EI, IN/OUT, ED prefix, EX (SP),HL,
# JP P (Z80IDLE hook), JR C,$-3 and DJNZ $ (Z80IDLE hooks), XY ops on IXh/IXl,
# XY prefixed jumps/calls, most DDCB/FDCB except BIT/RES/SET on (IX+d).
# Memory: z80_read_map / z80_write_map like picodrive_read / WRITE_MEM8: a
# direct page -> load/store; a handler -> called with CPU->ICount current.
# CPU->Status can't change while the Z80 runs (only pico_cmn.c's frame loop
# raises Z80 interrupts), so it is not re-checked between opcodes.
# No lp, no gp.
import os

A, F, BC, DE, HL, SP = 'r18', 'r19', 'r20', 'r21', 'r22', 'r24'
# cz80_struc offsets (z80x_glue checks them with static asserts in cz80.c)
O = dict(BC=0, DE=2, HL=4, FA=6, IX=8, IY=10, SP=12, BC2=0x14, DE2=0x16, HL2=0x18, FA2=0x1a,
         R=0x1c, ICOUNT=0x2c, BASEPC=0x34, FETCH=0x3c)
CF, NF, PF, XF, HF, YF, ZF, SF = 1, 2, 4, 8, 16, 32, 64, 128

out = []
def E(*s): out.extend(s)

lab_n = [0]
def L():
    lab_n[0] += 1
    return '.L%d' % lab_n[0]

class H:
    """one handler: body lines + cold stubs"""
    def __init__(s, name):
        s.name = name; s.b = []; s.cold = []
    def e(s, *ls):
        for l in ls: s.b.append('\t' + l if not l.endswith(':') else l)
    def rd8(s, a):
        """r0 = byte at 16-bit clean address reg a (clobbers r0-r3, r12, blink on slow path)"""
        # Z80 RAM (0000-3fff, mirrored) directly; the rest through z80_read_map (.Lrds)
        l = L()
        s.e('brhs\t%s,0x4000,%ss' % (a, l), 'bmsk\tr1,%s,12' % a, 'ldb\tr0,[r25,r1]', l + ':')
        s.cold += [l + 's:'] + (['\tmov\tr0,%s' % a] if a != 'r0' else []) + ['\tbl\t.Lrds', '\tb\t' + l]
    def wr8(s, a, v):
        """byte reg v (clean) to address reg a; a != r1, v != r0; v, a not r2"""
        assert a not in ('r1', 'r2') and v not in ('r0', 'r2'), (a, v)
        l = L()
        s.e('brhs\t%s,0x4000,%ss' % (a, l), 'bmsk\tr2,%s,12' % a, 'add\tr2,r2,r25', 'stb\t%s,[r2]' % v, l + ':')
        s.cold += [l + 's:'] + (['\tmov\tr1,%s' % v] if v != 'r1' else []) + \
                  (['\tmov\tr0,%s' % a] if a != 'r0' else []) + ['\tbl\t.Lwrs', '\tb\t' + l]
    def arg8(s, d):
        s.e('ldb\t%s,[r14]' % d, 'add\tr14,r14,1')
    def arg16(s, d, t='r3'):
        s.e('ldb\t%s,[r14]' % d, 'ldb\t%s,[r14,1]' % t, 'add\tr14,r14,2', 'asl\t%s,%s,8' % (t, t), 'or\t%s,%s,%s' % (d, d, t))
    def argd(s, d):
        """signed displacement"""
        s.e('ldb.x\t%s,[r14]' % d, 'add\tr14,r14,1')
    def next(s, cyc):
        s.e('ldb\tr0,[r14]', 'sub.f\tr15,r15,%d' % cyc, 'ble\t.Lexit', 'ld.as\tr1,[r17,r0]',
            'add\tr14,r14,1', 'add\tr23,r23,1', 'j\t[r1]')
    def unsup(s, nbytes, nr, cyc=0):
        s.e('sub\tr14,r14,%d' % nbytes, 'sub\tr23,r23,%d' % nr)
        if cyc: s.e('add\tr15,r15,%d' % cyc)
        s.e('b\t.Lexit')
    def setpc(s, a):
        """PC = a (16-bit clean, not r1): BasePC = Fetch[a >> 10]"""
        s.e('lsr\tr1,%s,10' % a, 'add2\tr1,r13,r1', 'ld\tr16,[r1,%d]' % O['FETCH'], 'add\tr14,%s,r16' % a)
    def push16(s, v):
        """push 16-bit clean reg v (must survive the first write: fp or a fixed reg)"""
        s.e('sub\t%s,%s,2' % (SP, SP), 'bmsk\t%s,%s,15' % (SP, SP), 'extb\tr4,%s' % v)
        # WRITE_MEM16(sp, v): low byte at sp, then high at sp + 1 (16-bit wrap)
        s.e('mov\tr5,%s' % SP)
        s.wr8('r5', 'r4')
        s.e('add\tr5,%s,1' % SP, 'bmsk\tr5,r5,15', 'lsr\tr4,%s,8' % v)
        s.wr8('r5', 'r4')
    def pop16(s, d):
        """d = pop (d: fixed reg or fp; byte reads at sp, sp + 1)"""
        s.rd8(SP)
        s.e('mov\t%s,r0' % d, 'add\tr4,%s,1' % SP, 'bmsk\tr4,r4,15')
        s.rd8('r4')
        s.e('asl\tr0,r0,8', 'or\t%s,%s,r0' % (d, d), 'add\t%s,%s,2' % (SP, SP), 'bmsk\t%s,%s,15' % (SP, SP))
    def text(s):
        return [s.name + ':'] + s.b + s.cold

# 8-bit register access (index: B C D E H L (HL) A)
R16OF = {0: BC, 1: BC, 2: DE, 3: DE, 4: HL, 5: HL}
def get8(h, i, d):
    if i == 7: h.e('mov\t%s,%s' % (d, A))
    elif i % 2 == 0: h.e('lsr\t%s,%s,8' % (d, R16OF[i]))
    else: h.e('extb\t%s,%s' % (d, R16OF[i]))
def set8(h, i, v, t='r2'):
    """register i = clean byte v (v != t)"""
    if i == 7: h.e('mov\t%s,%s' % (A, v)); return
    r = R16OF[i]
    if i % 2 == 0: h.e('extb\t%s,%s' % (r, r), 'asl\t%s,%s,8' % (t, v), 'or\t%s,%s,%s' % (r, r, t))
    else: h.e('lsr\t%s,%s,8' % (r, r), 'asl\t%s,%s,8' % (r, r), 'or\t%s,%s,%s' % (r, r, v))

# ALU on A with clean byte v (not r0-r3): flags exactly as Cz80's tables
def alu(h, k, v):
    if k in (0, 1, 2, 3, 7):       # add adc sub sbc cp
        op = 'add' if k < 2 else 'sub'
        if k in (1, 3):
            h.e('and\tr3,%s,1' % F, '%s\tr2,%s,%s' % (op, A, v), '%s\tr2,r2,r3' % op)
        else:
            h.e('%s\tr2,%s,%s' % (op, A, v))
        h.e('xor\tr3,%s,%s' % (A, v), 'xor\tr3,r3,r2', 'and\tr3,r3,%d' % HF,   # H
            'lsr\tr0,r2,8', 'and\tr0,r0,1', 'or\tr3,r3,r0')                     # C
        if k < 2: h.e('xor\tr0,%s,r2' % A, 'xor\tr1,%s,r2' % v)            # V add: (A^r)&(v^r)
        else: h.e('xor\tr0,%s,%s' % (A, v), 'xor\tr1,%s,r2' % A)           # V sub: (A^v)&(A^r)
        h.e('and\tr0,r0,r1', 'and\tr0,r0,0x80', 'lsr\tr0,r0,5', 'or\tr3,r3,r0', 'extb\tr2,r2')
        if k >= 2: h.e('or\tr3,r3,%d' % NF)
        if k == 7:
            h.e('ldb\tr0,[r2,z80x_sz]', 'and\tr0,r0,%d' % (SF | ZF), 'or\tr3,r3,r0',
                'and\tr0,%s,%d' % (v, YF | XF), 'or\t%s,r3,r0' % F)
        else:
            h.e('ldb\tr0,[r2,z80x_sz]', 'mov\t%s,r2' % A, 'or\t%s,r3,r0' % F)
    elif k == 4:
        h.e('and\t%s,%s,%s' % (A, A, v), 'ldb\tr0,[%s,z80x_szp]' % A, 'or\t%s,r0,%d' % (F, HF))
    elif k == 5:
        h.e('xor\t%s,%s,%s' % (A, A, v), 'ldb\t%s,[%s,z80x_szp]' % (F, A))
    elif k == 6:
        h.e('or\t%s,%s,%s' % (A, A, v), 'ldb\t%s,[%s,z80x_szp]' % (F, A))

COND = {0: ('Z', 0), 1: ('Z', 1), 2: ('C', 0), 3: ('C', 1), 4: ('P', 0), 5: ('P', 1), 6: ('S', 0), 7: ('S', 1)}
CBIT = {'Z': 6, 'C': 0, 'P': 2, 'S': 7}
def park_jp(h):
    """-DZ80PARK: a taken backward JP (target r4 < PC after it) goes to Cz80 (its hook, z80park.h)"""
    l = L()
    l2 = L(); l3 = L()
    h.e('#ifdef Z80PARK', 'sub	r6,r14,r16', 'bmsk	r6,r6,15', 'brlo	r4,r6,%s' % l, l3 + ':', '#endif')
    # backward: to Cz80 if the target is the watched loop head, or every 64th time
    h.cold += ['#ifdef Z80PARK', l + ':', '	ld	r5,[z80pk_watch]', '	breq	r4,r5,%s' % l2,
               '	ld	r5,[z80pk_ctr]', '	sub.f	r5,r5,1', '	st	r5,[z80pk_ctr]', '	bgt	' + l3,
               l2 + ':', '	sub	r14,r14,3', '	sub	r23,r23,1', '	b	.Lexit', '#endif']

def cond_skip(h, c, lab):
    """branch to lab if condition c is FALSE"""
    f, want = COND[c]
    h.e('%s\t%s,%d,%s' % ('bbit0' if want else 'bbit1', F, CBIT[f], lab))

mtab = {}; cb = {}; xy = {'ix': {}, 'iy': {}}; xycb = {'ix': {}, 'iy': {}}

def gen_main():
    for op in range(256):
        h = H('zh_m%02x' % op)
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        ok = True
        if op == 0x00: h.next(4)
        elif x == 1 and op != 0x76:            # LD r,r'
            if y == z: h.next(4)
            elif z == 6:
                h.rd8(HL); set8(h, y, 'r0'); h.next(7)
            elif y == 6:
                get8(h, z, 'r4'); h.wr8(HL, 'r4'); h.next(7)
            else:
                get8(h, z, 'r4'); set8(h, y, 'r4'); h.next(4)
        elif x == 0 and z == 6:                # LD r,n
            if y == 6:
                h.arg8('r4'); h.wr8(HL, 'r4'); h.next(10)
            else:
                h.arg8('r4'); set8(h, y, 'r4'); h.next(7)
        elif x == 0 and z == 1 and q == 0:     # LD rr,nn
            h.arg16('r4'); h.e('mov\t%s,r4' % [BC, DE, HL, SP][p]); h.next(10)
        elif op in (0x0a, 0x1a):
            h.rd8([BC, DE][p]); h.e('mov\t%s,r0' % A); h.next(7)
        elif op in (0x02, 0x12):
            h.wr8([BC, DE][p], A); h.next(7)
        elif op == 0x3a:
            h.arg16('r4'); h.rd8('r4'); h.e('mov\t%s,r0' % A); h.next(13)
        elif op == 0x32:
            h.arg16('r4'); h.wr8('r4', A); h.next(13)
        elif op == 0x2a:                       # LD HL,(nn)
            h.arg16('fp'); h.rd8('fp'); h.e('mov\t%s,r0' % HL, 'add\tr4,fp,1', 'bmsk\tr4,r4,15'); h.rd8('r4')
            h.e('asl\tr0,r0,8', 'or\t%s,%s,r0' % (HL, HL)); h.next(16)
        elif op == 0x22:                       # LD (nn),HL
            h.arg16('fp'); h.e('extb\tr4,%s' % HL); h.wr8('fp', 'r4')
            h.e('add\tr5,fp,1', 'bmsk\tr5,r5,15', 'lsr\tr4,%s,8' % HL); h.wr8('r5', 'r4'); h.next(16)
        elif x == 0 and z == 3:                # INC/DEC rr
            r = [BC, DE, HL, SP][p]
            h.e('%s\t%s,%s,1' % ('sub' if q else 'add', r, r), 'bmsk\t%s,%s,15' % (r, r)); h.next(6)
        elif x == 0 and z in (4, 5):           # INC/DEC r
            t = 'z80x_inc' if z == 4 else 'z80x_dec'
            if y == 6:
                h.rd8(HL); h.e('%s\tr4,r0,1' % ('add' if z == 4 else 'sub'), 'extb\tr4,r4',
                               'and\t%s,%s,1' % (F, F), 'ldb\tr0,[r4,%s]' % t, 'or\t%s,%s,r0' % (F, F))
                h.wr8(HL, 'r4'); h.next(11)
            else:
                get8(h, y, 'r4'); h.e('%s\tr4,r4,1' % ('add' if z == 4 else 'sub'), 'extb\tr4,r4',
                                      'and\t%s,%s,1' % (F, F), 'ldb\tr0,[r4,%s]' % t, 'or\t%s,%s,r0' % (F, F))
                set8(h, y, 'r4'); h.next(4)
        elif x == 0 and z == 1 and q == 1:     # ADD HL,rr
            v = [BC, DE, HL, SP][p]
            h.e('add\tr2,%s,%s' % (HL, v), 'xor\tr3,%s,%s' % (HL, v), 'xor\tr3,r3,r2', 'lsr\tr3,r3,8',
                'and\tr3,r3,%d' % HF, 'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,r3' % (F, F),
                'lsr\tr3,r2,16', 'or\t%s,%s,r3' % (F, F), 'lsr\tr3,r2,8', 'and\tr3,r3,%d' % (YF | XF),
                'or\t%s,%s,r3' % (F, F), 'bmsk\t%s,r2,15' % HL); h.next(11)
        elif op == 0x07:   # RLCA
            h.e('lsr\tr0,%s,7' % A, 'asl\t%s,%s,1' % (A, A), 'or\t%s,%s,r0' % (A, A), 'extb\t%s,%s' % (A, A),
                'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'and\tr0,%s,%d' % (A, YF | XF | CF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x0f:   # RRCA
            h.e('and\tr0,%s,1' % A, 'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,r0' % (F, F),
                'asl\tr0,r0,7', 'lsr\t%s,%s,1' % (A, A), 'or\t%s,%s,r0' % (A, A),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x17:   # RLA
            h.e('lsr\tr1,%s,7' % A, 'and\tr0,%s,1' % F, 'asl\t%s,%s,1' % (A, A), 'or\t%s,%s,r0' % (A, A),
                'extb\t%s,%s' % (A, A), 'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,r1' % (F, F),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x1f:   # RRA
            h.e('and\tr1,%s,1' % A, 'asl\tr0,%s,7' % F, 'lsr\t%s,%s,1' % (A, A), 'or\t%s,%s,r0' % (A, A),
                'extb\t%s,%s' % (A, A), 'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,r1' % (F, F),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x2f:   # CPL
            h.e('xor\t%s,%s,0xff' % (A, A), 'and\t%s,%s,%d' % (F, F, SF | ZF | PF | CF), 'or\t%s,%s,%d' % (F, F, HF | NF),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x37:   # SCF
            h.e('and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,1' % (F, F),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(4)
        elif op == 0x3f:   # CCF
            h.e('and\tr1,%s,1' % F, 'asl\tr1,r1,4', 'and\t%s,%s,%d' % (F, F, SF | ZF | PF | CF), 'or\t%s,%s,r1' % (F, F),
                'and\tr0,%s,%d' % (A, YF | XF), 'or\t%s,%s,r0' % (F, F), 'xor\t%s,%s,1' % (F, F)); h.next(4)
        elif op == 0x08:   # EX AF,AF'
            h.e('ldw\tr0,[r13,%d]' % O['FA2'], 'asl\tr1,%s,8' % F, 'or\tr1,r1,%s' % A, 'stw\tr1,[r13,%d]' % O['FA2'],
                'extb\t%s,r0' % A, 'lsr\t%s,r0,8' % F); h.next(4)
        elif op == 0xeb:
            h.e('mov\tr0,%s' % DE, 'mov\t%s,%s' % (DE, HL), 'mov\t%s,r0' % HL); h.next(4)
        elif op == 0xd9:   # EXX
            for r, o in ((BC, 'BC2'), (DE, 'DE2'), (HL, 'HL2')):
                h.e('ldw\tr0,[r13,%d]' % O[o], 'stw\t%s,[r13,%d]' % (r, O[o]), 'mov\t%s,r0' % r)
            h.next(4)
        elif op == 0xf9:
            h.e('mov\t%s,%s' % (SP, HL)); h.next(6)
        elif op == 0xe9:
            h.setpc(HL); h.next(4)
        elif x == 2:       # ALU A,r
            if z == 6:
                h.rd8(HL); h.e('mov\tr4,r0'); alu(h, y, 'r4'); h.next(7)
            else:
                if z == 7: v = A
                else: get8(h, z, 'r4'); v = 'r4'
                if v == A: h.e('mov\tr4,%s' % A); v = 'r4'
                alu(h, y, v); h.next(4)
        elif x == 3 and z == 6:                # ALU A,n
            h.arg8('r4'); alu(h, y, 'r4'); h.next(7)
        elif op == 0xc3:
            h.arg16('r4'); park_jp(h); h.setpc('r4'); h.next(10)
        elif x == 3 and z == 2:                # JP cc
            l = L(); cond_skip(h, y, l)
            if op == 0xf2:     # JP P taken: Cz80's Z80IDLE_JP (ld a,(hl) / or a / jp p,$-2)
                lj = L(); idle_pre(h, lj, 3, 0x1ffe)
                h.e('ldb	r1,[r14,-3]', 'brne	r1,0x7e,%s' % lj, 'ldb	r1,[r14,-2]', 'brne	r1,0xb7,%s' % lj,
                    'ldb	r1,[r14]', 'ldb	r3,[r14,1]', 'asl	r3,r3,8', 'or	r1,r1,r3', 'sub	r3,r2,3', 'brne	r1,r3,%s' % lj)
                idle_loop(h, 21, 3)
                h.e(lj + ':')
            if IDLE3 and op in (0xc2, 0xca):  # -DZ80IDLE3: JP NZ/Z,$-3 after BIT b,(HL) / LD A,r + OR r' / LD A,(HL) + OR A
                lj = L(); lc = L(); lf = L(); h.e('ldb	r1,[r14,-3]', 'breq	r1,0xcb,%s' % lc,
                    'sub	r1,r1,0x78', 'brlo	r1,8,%s' % lc, lj + ':')
                t = H('t'); t.e(lc + ':', 'ldb	r1,[r14]', 'ldb	r3,[r14,1]', 'asl	r3,r3,8', 'or	r1,r1,r3', 'sub	r2,r14,r16', 'sub	r3,r2,3', 'brne	r1,r3,%s' % lf)
                idle3(t, lf, op == 0xca, 22, 18); t.e(lf + ':', 'b	%s' % lj)
                h.cold += farbr(t.b)
            h.arg16('r4'); park_jp(h); h.setpc('r4'); h.next(10)
            h.e(l + ':', 'add	r14,r14,2'); h.next(10)
        elif op == 0x18:
            if IDLE3:  # -DZ80IDLE4: JR $-9 closing Technosoft's YM timer wait (z80_tfwait, ymwait.c)
                lj = L(); lc = L(); h.e('ldb	r1,[r14]', 'breq	r1,0xf5,%s' % lc, lj + ':')
                t = H('t'); t.e(lc + ':', 'sub	r1,r14,r16', 'brhs	r1,0x2000,%s' % lj,
                    'st.a	blink,[sp,-4]', 'st	r15,[r13,44]', 'mov	r0,r15', 'asl	r2,%s,8' % F, 'or	r2,r2,%s' % A, 'mov	r3,%s' % SP, 'bl	z80_tfwait',
                    'ld	r15,[r13,44]', 'asl	r1,r0,3', 'add	r23,r23,r1', 'add	r23,r23,r0', 'add	r23,r23,r0', 'add	r23,r23,r0',
                    'ld.ab	blink,[sp,4]', 'b	%s' % lj)
                h.cold += farbr(t.b)
            h.argd('r4'); h.e('add	r14,r14,r4'); h.next(12)
        elif op in (0x20, 0x28, 0x30, 0x38):   # JR cc
            c = {0x20: 0, 0x28: 1, 0x30: 2, 0x38: 3}[op]
            l = L(); cond_skip(h, c, l)
            if op == 0x38:     # JR C taken: Cz80's Z80IDLE_JRC (ld a,(hl) / cp n / jr c,$-3)
                lj = L(); h.e('ldb	r0,[r14]', 'brne	r0,0xfb,%s' % lj)
                idle_pre(h, lj, 4, 0x2000)
                h.e('ldb	r1,[r14,-4]', 'brne	r1,0x7e,%s' % lj, 'ldb	r1,[r14,-3]', 'brne	r1,0xfe,%s' % lj)
                idle_loop(h, 26, 3)
                h.e(lj + ':')
            if IDLE3 and op in (0x20, 0x28):  # -DZ80IDLE3: JR NZ/Z,$-3 (as JP above)
                lj = L(); lc = L(); lf = L(); h.e('ldb	r1,[r14]', 'breq	r1,0xfc,%s' % lc, lj + ':')
                t = H('t'); t.e(lc + ':', 'sub	r2,r14,r16')
                idle3(t, lf, op == 0x28, 24, 20); t.e(lf + ':', 'b	%s' % lj)
                h.cold += farbr(t.b)
            h.argd('r4'); h.e('add	r14,r14,r4'); h.next(12)
            h.e(l + ':', 'add	r14,r14,1'); h.next(7)
        elif op == 0x10:   # DJNZ (taken DJNZ $: Cz80's Z80IDLE_DJNZ)
            ln = L(); lj = L()
            h.e('lsr	r0,%s,8' % BC, 'breq	r0,1,%s' % ln, 'sub	r15,r15,1')
            h.e('sub	%s,%s,0x100' % (BC, BC), 'bmsk	%s,%s,15' % (BC, BC))
            h.e('ldb	r1,[r14]', 'brne	r1,0xfe,%s' % lj, 'ldb	r1,[r13,0x22]', 'brne	r1,0,%s' % lj,
                'sub	r2,r14,r16', 'brlo	r2,1,%s' % lj, 'brhs	r2,0x2000,%s' % lj)
            lp = L()
            # while (ICount >= 26 && B > 1) { ICount -= 13; B--; R++; }
            h.e(lp + ':', 'brlt	r15,26,%s' % lj, 'lsr	r0,%s,8' % BC, 'brls	r0,1,%s' % lj,
                'sub	r15,r15,13', 'sub	%s,%s,0x100' % (BC, BC), 'b.d	%s' % lp, 'add	r23,r23,1')
            h.e(lj + ':')
            h.argd('r4'); h.e('add	r14,r14,r4'); h.next(12)
            h.e(ln + ':', 'sub	%s,%s,0x100' % (BC, BC), 'add	r14,r14,1'); h.next(8)
        elif op == 0xcd:   # CALL
            h.arg16('r4'); h.e('mov\tfp,r4'); h.e('sub\tr6,r14,r16', 'bmsk\tr6,r6,15')
            push_tmp(h, 'r6'); h.setpc('fp'); h.next(17)
        elif x == 3 and z == 4:                # CALL cc
            l = L(); cond_skip(h, y, l)
            h.arg16('r4'); h.e('mov\tfp,r4', 'sub\tr6,r14,r16', 'bmsk\tr6,r6,15')
            push_tmp(h, 'r6'); h.setpc('fp'); h.next(17)
            h.e(l + ':', 'add\tr14,r14,2'); h.next(10)
        elif op == 0xc9:
            h.pop16('fp'); h.setpc('fp'); h.next(10)
        elif x == 3 and z == 0:                # RET cc
            l = L(); cond_skip(h, y, l)
            h.e('sub\tr15,r15,1'); h.pop16('fp'); h.setpc('fp'); h.next(10)
            h.e(l + ':'); h.next(5)
        elif x == 3 and z == 7:                # RST
            h.e('sub\tr6,r14,r16', 'bmsk\tr6,r6,15'); push_tmp(h, 'r6')
            h.e('mov\tfp,%d' % (op & 0x38)); h.setpc('fp'); h.next(11)
        elif x == 3 and z == 1 and q == 0:     # POP
            if p == 3:
                h.pop16('fp'); h.e('lsr\t%s,fp,8' % A, 'extb\t%s,fp' % F)
            else: h.pop16([BC, DE, HL][p])
            h.next(10)
        elif x == 3 and z == 5 and q == 0:     # PUSH
            if p == 3:
                h.e('asl\tfp,%s,8' % A, 'or\tfp,fp,%s' % F); h.push16('fp')
            else: h.push16([BC, DE, HL][p])
            h.next(11)
        elif op == 0xf3 and IDLE3:   # DI (-DZ80DIA; Cz80 runs the next opcode unchecked: only when > 4 cycles are left)
            lu = L(); lp = L(); lf = L(); ld = L()
            h.e('brle	r15,4,%s' % lu)
            h.e('#ifdef Z80POLL', 'b	%s' % lp, '#endif')
            h.e(ld + ':', 'mov	r0,0', 'stw	r0,[r13,0x1e]')
            h.next(4)
            h.e(lu + ':')
            h.unsup(1, 1)
            # Z80POLL_DI: di / push af / push hl / ld hl,nn / ld a,(hl) / or a / jr z,T ... T: pop hl / pop af
            # with (nn) == 0, all of it in this call (ICount > 69), Status 0, nn and SP in Z80 RAM: at once
            t = H('t')
            t.e(lp + ':', 'ldb	r1,[r13,0x22]', 'brne	r1,0,%s' % lf, 'brle	r15,69,%s' % lf,
                'ldb	r1,[r14]', 'brne	r1,0xf5,%s' % lf, 'ldb	r1,[r14,1]', 'brne	r1,0xe5,%s' % lf,
                'ldb	r1,[r14,2]', 'brne	r1,0x21,%s' % lf, 'ldb	r1,[r14,5]', 'brne	r1,0x7e,%s' % lf,
                'ldb	r1,[r14,6]', 'brne	r1,0xb7,%s' % lf, 'ldb	r1,[r14,7]', 'brne	r1,0x28,%s' % lf,
                'ldb	r2,[r14,3]', 'ldb	r1,[r14,4]', 'asl	r1,r1,8', 'or	r2,r2,r1', 'brhs	r2,0x2000,%s' % lf,
                'brlo	%s,8,%s' % (SP, lf), 'brhi	%s,0x2000,%s' % (SP, lf),
                'ldb.x	r3,[r14,8]', 'add	r3,r3,9', 'add	r4,r14,r3', 'ldb	r1,[r4]', 'brne	r1,0xe1,%s' % lf,
                'ldb	r1,[r4,1]', 'brne	r1,0xf1,%s' % lf, 'sub	r1,r4,r16', 'add	r1,r1,2', 'brhs	r1,0x2000,%s' % lf,
                'ldb	r1,[r25,r2]', 'brne	r1,0,%s' % lf,
                # WRITE_MEM16(SP - 2, A << 8 | F), WRITE_MEM16(SP - 4, HL), low byte first (SP in 8..0x2000)
                'add	r5,r25,%s' % SP, 'stb	%s,[r5,-2]' % F, 'stb	%s,[r5,-1]' % A,
                'stb	%s,[r5,-4]' % HL, 'lsr	r1,%s,8' % HL, 'stb	r1,[r5,-3]',
                'mov	r0,0', 'stw	r0,[r13,0x1e]', 'add	r23,r23,8', 'add	r14,r4,2')
            t.next(79)
            t.e(lf + ':', 'b	%s' % ld)
            h.cold += farbr(t.b)
        elif op == 0xfb:   # EI (-DZ80EI; else, with an IRQ pending when IFF1 was 0, or when the cycles run out: Cz80, which runs the next opcode even then)
            lu = L(); l1 = L()
            h.e('#ifdef Z80EI', 'brle	r15,4,%s' % lu, 'ldb\tr0,[r13,0x1e]', 'brne\tr0,0,%s' % l1, 'ld\tr0,[r13,0x28]', 'brne\tr0,0,%s' % lu,
                l1 + ':', 'mov\tr0,0x404', 'stw\tr0,[r13,0x1e]')
            h.next(4)
            h.e(lu + ':')
            h.unsup(1, 1)
            h.e('#else')
            h.unsup(1, 1)
            h.e('#endif')
        elif op == 0xcb:
            h.e('ldb\tr0,[r14]', 'add\tr14,r14,1', 'add\tr23,r23,1', 'ld.as\tr1,[z80x_cbtab,r0]', 'j\t[r1]')
        elif op in (0xdd, 0xfd):
            h.e('ldb\tr0,[r14]', 'sub\tr15,r15,4', 'add\tr14,r14,1', 'add\tr23,r23,1',
                'ld.as\tr1,[z80x_%stab,r0]' % ('ix' if op == 0xdd else 'iy'), 'j\t[r1]')
        else:
            ok = False
        if ok: mtab[op] = h

def idle_pre(h, lj, lo, hi):
    """Z80IDLE common checks (else to lj): Status 0, lo <= r_ = PC - BasePC < hi (r2), HL < 0x2000,
    A == Z80 RAM[HL]"""
    h.e('ldb	r1,[r13,0x22]', 'brne	r1,0,%s' % lj, 'sub	r2,r14,r16', 'brlo	r2,%d,%s' % (lo, lj),
        'brhs	r2,%d,%s' % (hi, lj), 'brhs	%s,0x2000,%s' % (HL, lj), 'ldb	r1,[r25,%s]' % HL,
        'brne	r1,%s,%s' % (A, lj))

import re as _re
def farbr(lines):
    """brCC a,b,lab -> cmp a,b + bCC lab (s21 range: the hook code is long)"""
    o = []
    for l in lines:
        m = _re.match(r'\s*br(eq|ne|lo|hs|lt|ge|le|gt|ls|hi)\s+([^,]+),([^,]+),(\S+)$', l)
        if m: o += ['	cmp	%s,%s' % (m.group(2), m.group(3)), '	b%s	%s' % (m.group(1), m.group(4))]
        else: o.append(l)
    return o

def idle3(h, lj, isz, per_bit, per_or):
    """Z80IDLE3 (the jump to the loop start, PC - 3 = the jump's address - 2, is taken; r2 = offset of the
    jump's operand): the loop is BIT b,(HL) (cycles per iteration per_bit), LD A,r + OR r' (A, B-E, H, L:
    registers only, per_or) or LD A,(HL) + OR A (per_or + 3). Nothing in it writes; Z80 RAM can't change
    inside one Exec call, so every later iteration is the same; the current flags must be what the loop
    computes now (the 68k may have written (HL) between two Exec calls). R += 3 an iteration."""
    lb = L(); lo = L(); lr = L()
    h.e('ldb	r1,[r13,0x22]', 'brne	r1,0,%s' % lj, 'brlo	r2,3,%s' % lj, 'brhs	r2,0x2000,%s' % lj,
        'ldb	r1,[r14,-3]', 'breq	r1,0xcb,%s' % lb, 'ldb	r0,[r14,-2]',
        'breq	r1,0x7e,%s' % lr,
        # LD A,r (78-7d, 7f) + OR r' (b0-b5, b7): the flags come from registers only
        'sub	r1,r1,0x78', 'brhs	r1,8,%s' % lj, 'breq	r1,6,%s' % lj,
        'sub	r0,r0,0xb0', 'brhs	r0,8,%s' % lj, 'breq	r0,6,%s' % lj)
    idle_loop(h, per_or, 3)
    h.e('b	%s' % lj)
    # LD A,(HL) + OR A: as Z80IDLE's ld a,(hl)/or a/jp p
    h.e(lr + ':', 'brne	r0,0xb7,%s' % lj, 'brhs	%s,0x2000,%s' % (HL, lj), 'ldb	r1,[r25,%s]' % HL, 'brne	r1,%s,%s' % (A, lj))
    idle_loop(h, per_or + 3, 3)
    h.e('b	%s' % lj)
    # BIT b,(HL): RAM[HL] bit b must still give the condition that took the jump
    ly = L()
    h.e(lb + ':', 'ldb	r0,[r14,-2]', 'and	r1,r0,0xc7', 'brne	r1,0x46,%s' % lj, 'brhs	%s,0x2000,%s' % (HL, ly),
        'lsr	r0,r0,3', 'and	r0,r0,7', 'ldb	r1,[r25,%s]' % HL, 'lsr	r1,r1,r0', 'and	r1,r1,1',
        ('brne	r1,0,%s' if isz else 'breq	r1,0,%s') % lj)
    idle_loop(h, per_bit, 3)
    h.e('b	%s' % lj)
    # BIT 0/1,(HL) on the YM2612 status (HL 4000-5fff), waiting for a timer flag (JR/JP Z): z80_ymwait (C)
    # skips the iterations whose status read still gives 0 (r0 = iterations, CPU->ICount updated)
    if isz:
        h.e(ly + ':', 'sub	r1,%s,0x4000' % HL, 'brhs	r1,0x2000,%s' % lj, 'lsr	r3,r0,3', 'and	r3,r3,7',
            'st.a	blink,[sp,-4]', 'mov	r0,r15', 'mov	r1,%d' % per_bit, 'mov	r2,%d' % (per_bit - 12), 'bl	z80_ymwait',
            'ld	r15,[r13,44]', 'add	r23,r23,r0', 'add	r23,r23,r0', 'add	r23,r23,r0', 'ld.ab	blink,[sp,4]')
    else:
        h.e(ly + ':')

def idle_loop(h, per, r):
    """m = (ICount - 1) / per whole iterations: ICount -= m * per, R += r * m"""
    lp = L(); ld = L()
    h.e(lp + ':', 'brle	r15,%d,%s' % (per, ld), 'sub	r15,r15,%d' % per, 'b.d	%s' % lp, 'add	r23,r23,%d' % r, ld + ':')

def push_tmp(h, v):
    """push a 16-bit value from a temp: kept in r7 across the first write? no: temps die, so
    write via the stack slot: the value is stored to [sp,0] first"""
    h.e('st\t%s,[sp,0]' % v)
    h.e('sub\t%s,%s,2' % (SP, SP), 'bmsk\t%s,%s,15' % (SP, SP), 'extb\tr4,%s' % v, 'mov\tr5,%s' % SP)
    h.wr8('r5', 'r4')
    h.e('ld\tr4,[sp,0]', 'add\tr5,%s,1' % SP, 'bmsk\tr5,r5,15', 'lsr\tr4,r4,8')
    h.wr8('r5', 'r4')

def gen_cb():
    for op in range(256):
        h = H('zh_cb%02x' % op)
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if z == 6: h.rd8(HL); src = 'r0'; h.e('mov\tr4,r0'); src = 'r4'
        else: get8(h, z, 'r4'); src = 'r4'
        if x == 0:     # rotates/shifts: res in r5 (8-bit), F = SZP[res] | carry
            if y == 6 and False: pass
            if y == 0: h.e('lsr\tr6,r4,7', 'asl\tr5,r4,1', 'or\tr5,r5,r6')            # RLC: c = src>>7
            elif y == 1: h.e('and\tr6,r4,1', 'asl\tr5,r4,7', 'lsr\tr1,r4,1', 'or\tr5,r5,r1')   # RRC
            elif y == 2: h.e('lsr\tr6,r4,7', 'and\tr1,%s,1' % F, 'asl\tr5,r4,1', 'or\tr5,r5,r1')   # RL
            elif y == 3: h.e('and\tr6,r4,1', 'asl\tr1,%s,7' % F, 'lsr\tr5,r4,1', 'or\tr5,r5,r1')   # RR
            elif y == 4: h.e('lsr\tr6,r4,7', 'asl\tr5,r4,1')                            # SLA
            elif y == 5: h.e('and\tr6,r4,1', 'and\tr1,r4,0x80', 'lsr\tr5,r4,1', 'or\tr5,r5,r1')   # SRA
            elif y == 6: h.e('lsr\tr6,r4,7', 'asl\tr5,r4,1', 'or\tr5,r5,1')             # SLL
            else: h.e('and\tr6,r4,1', 'lsr\tr5,r4,1')                                   # SRL
            h.e('extb\tr5,r5', 'ldb\tr0,[r5,z80x_szp]', 'or\t%s,r0,r6' % F)
            if z == 6: h.wr8(HL, 'r5'); h.next(15)
            else: set8(h, z, 'r5'); h.next(8)
        elif x == 1:   # BIT
            h.e('and\tr5,r4,%d' % (1 << y), 'and\t%s,%s,1' % (F, F), 'or\t%s,%s,%d' % (F, F, HF),
                'ldb\tr0,[r5,z80x_szbit]', 'or\t%s,%s,r0' % (F, F), 'and\t%s,%s,%d' % (F, F, 0xff & ~(XF | YF)))
            if z == 6: h.next(12)       # XY from 0xc0 & (XF|YF) = 0
            else: h.e('and\tr0,r4,%d' % (XF | YF), 'or\t%s,%s,r0' % (F, F)); h.next(8)
        else:          # RES / SET
            if x == 2: h.e('bclr\tr5,r4,%d' % y)
            else: h.e('bset\tr5,r4,%d' % y)
            if z == 6: h.wr8(HL, 'r5'); h.next(15)
            else: set8(h, z, 'r5'); h.next(8)
        cb[op] = h

# XY: ops that are the main op + 4 cycles (prefix already charged)
XY_PLAIN = set([0x41, 0x42, 0x43, 0x47, 0x48, 0x4a, 0x4b, 0x4f, 0x50, 0x51, 0x53, 0x57, 0x58, 0x59, 0x5a, 0x5f,
                0x78, 0x79, 0x7a, 0x7b, 0x40, 0x49, 0x52, 0x5b, 0x7f, 0x00,
                0x06, 0x0e, 0x16, 0x1e, 0x3e, 0x0a, 0x1a, 0x3a, 0x02, 0x12, 0x32, 0x01, 0x11, 0x31,
                0xc1, 0xd1, 0xf1, 0xc5, 0xd5, 0xf5, 0x08, 0xeb, 0xd9, 0x04, 0x0c, 0x14, 0x1c, 0x3c,
                0x05, 0x0d, 0x15, 0x1d, 0x3d, 0x03, 0x13, 0x33, 0x0b, 0x1b, 0x3b, 0x07, 0x0f, 0x17, 0x1f,
                0xc6, 0xce, 0xd6, 0xde, 0xe6, 0xee, 0xf6, 0xfe, 0x2f, 0x37, 0x3f])
for b in (0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8):
    for r in (0, 1, 2, 3, 7): XY_PLAIN.add(b + r)

def gen_xy(rn):
    o = O['IX' if rn == 'ix' else 'IY']
    T = xy[rn]
    def ea(h):     # fp = (XY + d) & 0xffff  (before d the prefix's 4 are charged)
        h.e('ldw\tr0,[r13,%d]' % o); h.argd('r1'); h.e('add\tfp,r0,r1', 'bmsk\tfp,fp,15')
    for op in range(256):
        h = H('zh_%s%02x' % (rn, op))
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        ok = True
        if op in XY_PLAIN: T[op] = None; continue
        if x == 1 and z == 6 and y != 6:      # LD r,(XY+d)
            ea(h); h.rd8('fp'); set8(h, y, 'r0'); h.next(15)
        elif x == 1 and y == 6 and z != 6:   # LD (XY+d),r
            ea(h); get8(h, z, 'r4'); h.wr8('fp', 'r4'); h.next(15)
        elif op == 0x36:
            ea(h); h.arg8('r4'); h.wr8('fp', 'r4'); h.next(15)
        elif op == 0x21:
            h.arg16('r4'); h.e('stw\tr4,[r13,%d]' % o); h.next(10)
        elif op == 0x2a:
            h.arg16('fp'); h.rd8('fp'); h.e('stw\tr0,[r13,%d]' % o, 'add\tr4,fp,1', 'bmsk\tr4,r4,15'); h.rd8('r4')
            h.e('ldw\tr1,[r13,%d]' % o, 'asl\tr0,r0,8', 'or\tr1,r1,r0', 'stw\tr1,[r13,%d]' % o); h.next(16)
        elif op == 0x22:
            h.arg16('fp'); h.e('ldb\tr4,[r13,%d]' % o); h.wr8('fp', 'r4')
            h.e('add\tr5,fp,1', 'bmsk\tr5,r5,15', 'ldb\tr4,[r13,%d]' % (o + 1)); h.wr8('r5', 'r4'); h.next(16)
        elif op == 0xe5:
            h.e('ldw\tr6,[r13,%d]' % o); push_tmp(h, 'r6'); h.next(11)
        elif op == 0xe1:
            h.pop16('fp'); h.e('stw\tfp,[r13,%d]' % o); h.next(10)
        elif op in (0x23, 0x2b):
            h.e('ldw\tr0,[r13,%d]' % o, '%s\tr0,r0,1' % ('add' if op == 0x23 else 'sub'), 'stw\tr0,[r13,%d]' % o); h.next(6)
        elif x == 0 and z == 1 and q == 1:     # ADD XY,rr
            h.e('ldw\tr4,[r13,%d]' % o)
            v = [BC, DE, 'r4', SP][p]
            h.e('add\tr2,r4,%s' % v, 'xor\tr3,r4,%s' % v, 'xor\tr3,r3,r2', 'lsr\tr3,r3,8',
                'and\tr3,r3,%d' % HF, 'and\t%s,%s,%d' % (F, F, SF | ZF | PF), 'or\t%s,%s,r3' % (F, F),
                'lsr\tr3,r2,16', 'or\t%s,%s,r3' % (F, F), 'lsr\tr3,r2,8', 'and\tr3,r3,%d' % (YF | XF),
                'or\t%s,%s,r3' % (F, F), 'stw\tr2,[r13,%d]' % o); h.next(11)
        elif x == 2 and z == 6:                # ALU A,(XY+d)
            ea(h); h.rd8('fp'); h.e('mov\tr4,r0', 'sub\tr15,r15,11'); alu(h, y, 'r4'); h.next(4)
        elif op in (0x34, 0x35):               # INC/DEC (XY+d)
            ea(h); h.e('sub\tr15,r15,8'); h.rd8('fp')
            t = 'z80x_inc' if op == 0x34 else 'z80x_dec'
            h.e('%s\tr4,r0,1' % ('add' if op == 0x34 else 'sub'), 'extb\tr4,r4',
                'and\t%s,%s,1' % (F, F), 'ldb\tr0,[r4,%s]' % t, 'or\t%s,%s,r0' % (F, F))
            h.wr8('fp', 'r4'); h.next(11)
        elif op == 0xe9:
            h.e('ldw\tr4,[r13,%d]' % o); h.setpc('r4'); h.next(4)
        elif op == 0xf9:
            h.e('ldw\t%s,[r13,%d]' % (SP, o)); h.next(6)
        elif op == 0xcb:   # XYCB: fp = address, then the op byte
            ea(h); h.e('ldb\tr0,[r14]', 'add\tr14,r14,1', 'add\tr23,r23,1',
                       'ld.as\tr1,[z80x_%scbtab,r0]' % rn, 'j\t[r1]')
        else: ok = False
        if ok: T[op] = h
    # XYCB: only (XY+d) forms without a register copy
    for op in range(256):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if z != 6 or x == 0: continue
        h = H('zh_%scb%02x' % (rn, op))
        h.rd8('fp')
        if x == 1:
            h.e('and\tr5,r0,%d' % (1 << y), 'and\t%s,%s,1' % (F, F), 'or\t%s,%s,%d' % (F, F, HF),
                'ldb\tr0,[r5,z80x_szbit]', 'and\tr0,r0,%d' % (0xff & ~(YF | XF)), 'or\t%s,%s,r0' % (F, F),
                'lsr\tr0,fp,8', 'and\tr0,r0,%d' % (YF | XF), 'or\t%s,%s,r0' % (F, F)); h.next(16)
        else:
            h.e('%s\tr5,r0,%d' % ('bclr' if x == 2 else 'bset', y)); h.wr8('fp', 'r5'); h.next(19)
        xycb[rn][op] = h

def tables():
    sz = []; szp = []; szbit = []; inc = []; dec = []
    for i in range(256):
        s = i & (SF | YF | XF)
        if not i: s |= ZF
        sb = (i & (SF | YF | XF)) | ((ZF | PF) if not i else 0)
        par = bin(i).count('1')
        sp = s | (PF if not (par & 1) else 0)
        iv = s | (PF if i == 0x80 else 0) | (HF if (i & 15) == 0 else 0)
        dv = s | NF | (PF if i == 0x7f else 0) | (HF if (i & 15) == 15 else 0)
        sz.append(s); szp.append(sp); szbit.append(sb); inc.append(iv); dec.append(dv)
    E('\t.section .rodata\n\t.align 4')
    for n, t in (('sz', sz), ('szp', szp), ('szbit', szbit), ('inc', inc), ('dec', dec)):
        E('z80x_%s:' % n)
        for i in range(0, 256, 16): E('\t.byte ' + ','.join('%d' % v for v in t[i:i + 16]))

FRAME = r'''
; z80x.S: generated by z80/mkz80x.py - do not edit
; r13 CPU  r14 PC (host)  r15 ICount  r16 BasePC  r17 main table  r18 A  r19 F
; r20 BC  r21 DE  r22 HL  r23 R (low byte counted)  r24 SP  r25 Z80 RAM
; fp: a value kept across memory accesses; [sp,0]: the same
	.text
	.global z80x_run
	.align 4
z80x_run:		; (cz80_struc *CPU, PC, zram) -> PC; caller: ICount > 0
	sub	sp,sp,64
	st	blink,[sp,4]
	st	r13,[sp,8]
	st	r14,[sp,12]
	st	r15,[sp,16]
	st	r16,[sp,20]
	st	r17,[sp,24]
	st	r18,[sp,28]
	st	r19,[sp,32]
	st	r20,[sp,36]
	st	r21,[sp,40]
	st	r22,[sp,44]
	st	r23,[sp,48]
	st	r24,[sp,52]
	st	r25,[sp,56]
	st	fp,[sp,60]
	mov	r13,r0
	mov	r14,r1
	ld	r15,[r13,%(ICOUNT)d]
	ld	r16,[r13,%(BASEPC)d]
	mov	r17,z80x_tab
	ldb	r18,[r13,%(FA)d]
	ldb	r19,[r13,%(FA)d+1]
	ldw	r20,[r13,%(BC)d]
	ldw	r21,[r13,%(DE)d]
	ldw	r22,[r13,%(HL)d]
	ldb	r23,[r13,%(R)d]
	ldw	r24,[r13,%(SP)d]
	mov	r25,r2
	ldb	r0,[r14]
	ld.as	r1,[r17,r0]
	add	r14,r14,1
	add	r23,r23,1
	j	[r1]

.Lexit:
	st	r15,[r13,%(ICOUNT)d]
	st	r16,[r13,%(BASEPC)d]
	stb	r18,[r13,%(FA)d]
	stb	r19,[r13,%(FA)d+1]
	stw	r20,[r13,%(BC)d]
	stw	r21,[r13,%(DE)d]
	stw	r22,[r13,%(HL)d]
	stb	r23,[r13,%(R)d]
	stw	r24,[r13,%(SP)d]
	mov	r0,r14
	ld	blink,[sp,4]
	ld	r13,[sp,8]
	ld	r14,[sp,12]
	ld	r15,[sp,16]
	ld	r16,[sp,20]
	ld	r17,[sp,24]
	ld	r18,[sp,28]
	ld	r19,[sp,32]
	ld	r20,[sp,36]
	ld	r21,[sp,40]
	ld	r22,[sp,44]
	ld	r23,[sp,48]
	ld	r24,[sp,52]
	ld	r25,[sp,56]
	ld	fp,[sp,60]
	j.d	[blink]
	add	sp,sp,64

; slow read: r0 address (not Z80 RAM): z80_read_map page or handler; returns r0 = byte
.Lrds:
	lsr	r1,r0,10
	ld.as	r1,[z80_read_map,r1]
	brlt	r1,0,1f
	asl	r1,r1,1
	j.d	[blink]
	ldb	r0,[r1,r0]
1:	st.a	blink,[sp,-4]
	st	r15,[r13,%(ICOUNT)d]
	asl	r1,r1,1
	jl	[r1]
	extb	r0,r0
	ld	r15,[r13,%(ICOUNT)d]
	ld.ab	blink,[sp,4]
	j	[blink]
; slow write: r0 address, r1 byte
.Lwrs:
	lsr	r2,r0,10
	ld.as	r2,[z80_write_map,r2]
	brlt	r2,0,1f
	add1	r2,r0,r2
	j.d	[blink]
	stb	r1,[r2]
1:	st.a	blink,[sp,-4]
	st	r15,[r13,%(ICOUNT)d]
	asl	r2,r2,1
	jl	[r2]
	ld	r15,[r13,%(ICOUNT)d]
	ld.ab	blink,[sp,4]
	j	[blink]

.Lu1:	; opcode left to Cz80
	sub	r14,r14,1
	b.d	.Lexit
	sub	r23,r23,1
.Lu2:	; prefixed (CB never: all done; DD/FD: 2 bytes, R 2, the prefix's 4 cycles)
	sub	r14,r14,2
	sub	r23,r23,2
	b.d	.Lexit
	add	r15,r15,4
.Lu4:	; DDCB/FDCB d op
	sub	r14,r14,4
	sub	r23,r23,3
	b.d	.Lexit
	add	r15,r15,4
''' % O

IDLE3 = False
I3OPS = (0x20, 0x28, 0xc2, 0xca)
def main():
    global IDLE3, mtab
    gen_main(); gen_cb(); gen_xy('ix'); gen_xy('iy')
    keep = mtab; mtab = {}; IDLE3 = True; gen_main(); i3 = mtab; mtab = keep; IDLE3 = False
    E(FRAME)
    for d in (mtab, cb, xycb['ix'], xycb['iy']):
        for op in sorted(d):
            E('\t.align 4'); out.extend(d[op].text())
    for rn in ('ix', 'iy'):
        for op in sorted(xy[rn]):
            h = xy[rn][op]
            if h is not None: E('\t.align 4'); out.extend(h.text())
    # alternative handlers (Z80IDLE3: I3OPS; Z80DIA: DI) in .xtext, out of the window: the same layout
    ALT = I3OPS + (0xf3, 0x18)
    E('#if defined(Z80IDLE3) || defined(Z80DIA) || defined(Z80IDLE4)', '\t.section .xtext.z80i,"ax",@progbits')
    for op in ALT:
        i3[op].name += 'i'; E('\t.align 4'); out.extend(i3[op].text())
    E('\t.text', '#endif')
    for op in ALT:
        fl = 'Z80DIA' if op == 0xf3 else 'Z80IDLE4' if op == 0x18 else 'Z80IDLE3'
        E('#ifdef ' + fl, '\t.set zt_%02x,%s' % (op, i3[op].name), '#else', '\t.set zt_%02x,%s' % (op, mtab[op].name if op in mtab else '.Lu1'), '#endif')
    tables()
    E('\t.align 4\nz80x_tab:')
    for i in range(0, 256, 8):
        E('\t.word ' + ','.join('zt_%02x' % o if o in ALT else mtab[o].name if o in mtab else '.Lu1' for o in range(i, i + 8)))
    E('z80x_cbtab:')
    for i in range(0, 256, 8): E('\t.word ' + ','.join(cb[o].name for o in range(i, i + 8)))
    for rn in ('ix', 'iy'):
        E('z80x_%stab:' % rn)
        names = []
        for o in range(256):
            if o in xy[rn]: names.append(mtab[o].name if xy[rn][o] is None else xy[rn][o].name)
            else: names.append('.Lu2')
        for i in range(0, 256, 8): E('\t.word ' + ','.join(names[i:i + 8]))
        E('z80x_%scbtab:' % rn)
        for i in range(0, 256, 8): E('\t.word ' + ','.join(xycb[rn][o].name if o in xycb[rn] else '.Lu4' for o in range(i, i + 8)))
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'z80x.S')
    open(p, 'w', newline='\n').write('\n'.join(out) + '\n')
    print("z80x.S: main %d cb %d ix %d ixcb %d" % (len(mtab), len(cb), sum(1 for v in xy['ix'].values()), len(xycb['ix'])))

main()
