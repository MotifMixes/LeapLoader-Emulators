# fam68b.py: more 68000 families of k68i.S: shifts, bit ops, MULU/MULS, DIVU,
# EXG, CMPM (templates in ops68.py)
from ops68 import *

def z_only(g, a, b):
    """Z = !(a & b); N, V, C, X kept (bit tests)"""
    n, e = g.lab(), g.lab()
    g.e('lsr\tr1,r20,31', 'lsr\tr2,r22,3', 'or\tr1,r1,r2', 'and\tr1,r1,1', 'bclr\tr22,r22,3')
    g.e('and.f\t0,%s,%s' % (a, b), 'beq\t%df' % n)
    g.e('asl\tr20,r1,31', 'b.d\t%df' % e, 'or\tr20,r20,1')
    g.at(n)
    g.e('mov\tr20,0', 'asl\tr1,r1,3', 'or\tr22,r22,r1')
    g.at(e)

def setx_from_c(g):
    """r22 = C (bit 0, in r22 already, other bits 0) | X = C"""
    g.e('asl\tr1,r22,4', 'or\tr22,r22,r1')

# FAME's shift rules (famec_opcodes.h), N = operand bits, c = count (1-8 or Dx & 63):
#  ASR c >= N: all sign bits, C = X = sign. LSR c > N: 0, C = X = 0.
#  ASL (register count) c >= N: 0, C = X = 0, V = (src != 0); ASL #8 (byte): C = X = bit 0.
#  LSL c > N: 0, C = X = 0. ROR/ROL: C = MSB/LSB of the result, X kept, V = 0.
def shift_body(g, typ, left, sz, c):
    """value in r4 (raw), c: int (immediate) or 'r5' (count 1-63);
    result (low sz bytes) -> r0, flags set"""
    N = 8 * sz; S = 32 - N
    dyn = not isinstance(c, int)
    top = ('asl\tr20,r0,%d' % S) if S else 'mov\tr20,r0'
    if typ == 0 and not left:      # ASR
        if sz < 4: g.e('%s\tr4,r4' % ('sexb' if sz == 1 else 'sexw'))
        if dyn:
            g.e('mov\tr1,r5', 'cmp\tr1,%d' % N, 'mov.hi\tr1,%d' % N, 'sub\tr2,r1,1', 'asr\tr2,r4,r2')
        else:
            g.e('asr\tr2,r4,%d' % (c - 1))
        g.e('asr\tr0,r2,1', 'and\tr22,r2,1'); setx_from_c(g); g.e(top)
        return
    if typ == 1 and not left:      # LSR
        if sz < 4: g.e('%s\tr4,r4' % ('extb' if sz == 1 else 'extw'))
        if dyn:
            n = g.lab()
            g.e('mov\tr2,0', 'cmp\tr5,%d' % N, 'bhi\t%df' % n, 'sub\tr2,r5,1', 'lsr\tr2,r4,r2')
            g.at(n)
        else:
            g.e('lsr\tr2,r4,%d' % (c - 1))
        g.e('lsr\tr0,r2,1', 'and\tr22,r2,1'); setx_from_c(g); g.e(top)
        return
    if typ in (0, 1):              # ASL / LSL, on the operand moved to the top
        if S: g.e('asl\tr4,r4,%d' % S)
        if not dyn and c == N:     # ASL.B / LSL.B #8: 0, C = X = bit 0, V (ASL) = src != 0
            g.e('mov\tr20,0', 'mov\tr0,0', 'lsr\tr2,r4,24', 'and\tr22,r2,1'); setx_from_c(g)
            if typ == 0: g.e('cmp\tr4,0', 'or.ne\tr22,r22,2')
            return
        if dyn:
            nbig = g.lab()
            g.e('cmp\tr5,%d' % N, '%s\t%df' % ('bhs' if typ == 0 else 'bhi', nbig))
            g.e('sub\tr1,r5,1', 'asl\tr2,r4,r1')
        else:
            g.e('asl\tr2,r4,%d' % (c - 1))
        g.e('asl\tr20,r2,1', 'lsr\tr22,r2,31'); setx_from_c(g)
        if typ == 0:   # V: the top c+1 bits of the operand not all equal
            if dyn: g.e('rsub\tr1,r5,31', 'asr\tr1,r4,r1')
            else: g.e('asr\tr1,r4,%d' % (31 - c))
            g.e('add\tr1,r1,1', 'cmp\tr1,1', 'or.hi\tr22,r22,2')
        g.e(('lsr\tr0,r20,%d' % S) if S else 'mov\tr0,r20')
        if dyn:
            nend = g.lab()
            g.e('b\t%df' % nend)
            g.at(nbig)
            g.e('mov\tr20,0', 'mov\tr0,0', 'mov\tr22,0')
            if typ == 0: g.e('cmp\tr4,0', 'mov.ne\tr22,2')
            g.at(nend)
        return
    # ROR / ROL: replicate the operand so that a 32-bit rotate works
    if sz == 1: g.e('extb\tr4,r4', 'asl\tr1,r4,8', 'or\tr4,r4,r1', 'asl\tr1,r4,16', 'or\tr4,r4,r1')
    elif sz == 2: g.e('extw\tr4,r4', 'asl\tr1,r4,16', 'or\tr4,r4,r1')
    if dyn:
        g.e('and\tr1,r5,%d' % (N - 1))
        if left: g.e('rsub\tr1,r1,32', 'and\tr1,r1,31')
        g.e('ror\tr0,r4,r1')
    else:
        k = c % N
        if k: g.e('ror\tr0,r4,%d' % ((32 - k) if left else k))
        else: g.e('mov\tr0,r4')
    g.e(top, 'and\tr22,r22,0x10')
    if left: g.e('and\tr1,r0,1', 'or\tr22,r22,r1')
    else: g.e('lsr\tr1,r20,31', 'or\tr22,r22,r1')

def f_shift():
    for cnt in range(8):
        for left in (0, 1):
            for s in range(3):
                sz = (1, 2, 4)[s]
                for ir in (0, 1):
                    for typ in (0, 1, 3):
                        grp = (0xE000 | (cnt << 9) | (left << 8) | (s << 6) | (ir << 5) | (typ << 3)) >> 3
                        op0 = grp << 3
                        rs = rets(op0)
                        if not legal(op0) or len(set(rs)) != 1:
                            fails.append('%04x: shift rets %s' % (op0, rs)); continue
                        g = G(op0)
                        ld_dn(g, 'S', 'r4')
                        if ir == 0:
                            c = cnt or 8
                            shift_body(g, typ, left, sz, c)
                            st_dn(g, 'S', 'r0', sz)
                            g.next(rs[0] + 2 * c)
                        else:
                            nz = g.lab()
                            g.e('ld\tr5,[r13,%d]' % (cnt * 4), 'and.f\tr5,r5,63', 'bne\t%df' % nz)
                            flags_logic(g, 'r4', sz)
                            g.next(rs[0])
                            g.at(nz)
                            shift_body(g, typ, left, sz, 'r5')
                            st_dn(g, 'S', 'r0', sz)
                            g.e('asl\tr0,r5,1', 'add\tr0,r0,%d' % rs[0])
                            g.nextr('r0')
                        put(grp, g)
    for left in (0, 1):            # memory: word, by 1
        for typ in (0, 1, 3):
            for m in range(2, 8):
                grp = (0xE0C0 | (typ << 9) | (left << 8) | (m << 3)) >> 3
                def body(g, ea, op, typ=typ, left=left):
                    c = same_cyc(op, ea)
                    ea_addr(g, ea, 2, 'r6'); g.e('mov\tr0,r6'); rd(g, 2, 'r4')
                    shift_body(g, typ, left, 2, 1)
                    wr(g, 2, 'r6', 'r0'); g.next(c)
                try_gen(grp, body, m, (0, 1))

def f_bitops():
    for dyn in (1, 0):
        for typ in range(4):          # BTST BCHG BCLR BSET
            for m in range(8):
                if m == 1: continue
                base = (0x0100 if dyn else 0x0800) | (typ << 6) | (m << 3)
                for dr in (range(8) if dyn else [0]):
                    grp = (base | (dr << 9)) >> 3
                    def body(g, ea, op, typ=typ, dyn=dyn):
                        rs = rets(op)
                        extra = 'io_cycle_counter' in '\n'.join(H[T[op]])
                        if len(rs) != 1: raise Fail('bitop rets')
                        if dyn: ld_dn(g, 'D', 'r5')
                        else: fetch_ext(g, 'r5', 1)
                        if ea[0] == 0:
                            g.e('and\tr5,r5,31', 'mov\tr1,1', 'asl\tr5,r1,r5')
                            ld_dn(g, ea[1], 'r4')
                            z_only(g, 'r4', 'r5')
                            if typ:
                                g.e({1: 'xor', 2: 'bic', 3: 'or'}[typ] + '\tr4,r4,r5')
                                st_dn(g, ea[1], 'r4')
                            if not extra: g.next(rs[0]); return
                            n = g.lab()     # FAME (cyclone timing): bit >= 16 costs 2 more
                            g.e('lsr.f\tr0,r5,16', 'beq\t%df' % n); g.next(rs[0] + 2); g.at(n); g.next(rs[0])
                            return
                        if extra: raise Fail('bitop mem cycles')
                        if ea == (7, 4):     # BTST Dn,#imm
                            g.e('and\tr5,r5,7', 'mov\tr1,1', 'asl\tr5,r1,r5')
                            fetch_ext(g, 'r4', 1); z_only(g, 'r4', 'r5'); g.next(rs[0]); return
                        g.e('and\tr5,r5,7', 'mov\tr1,1', 'asl\tr5,r1,r5')
                        ea_addr(g, ea, 1, 'r6'); g.e('mov\tr0,r6'); rd(g, 1, 'r4')
                        z_only(g, 'r4', 'r5')
                        if typ:
                            g.e({1: 'xor', 2: 'bic', 3: 'or'}[typ] + '\tr1,r4,r5')
                            wr(g, 1, 'r6', 'r1')
                        g.next(rs[0])
                    try_gen(grp, body, m, (0, 1, 2, 3, 4) if (typ == 0 and dyn) else ((0, 1, 2, 3) if typ == 0 else (0, 1)))

def popcount(g, r):
    """r = number of set bits of r (FAME's BITCOUNT); clobbers r1, r2"""
    g.e('lsr\tr1,%s,1' % r, 'and\tr1,r1,0x55555555', 'sub\t%s,%s,r1' % (r, r),
        'and\tr1,%s,0x33333333' % r, 'lsr\tr2,%s,2' % r, 'and\tr2,r2,0x33333333',
        'add\t%s,r1,r2' % r, 'lsr\tr1,%s,4' % r, 'add\t%s,%s,r1' % (r, r),
        'and\t%s,%s,0x0f0f0f0f' % (r, r), 'lsr\tr1,%s,8' % r, 'add\t%s,%s,r1' % (r, r),
        'lsr\tr1,%s,16' % r, 'add\t%s,%s,r1' % (r, r), 'and\t%s,%s,63' % (r, r))

def f_muldiv():
    for signed in (0, 1):
        for m in range(8):
            if m == 1: continue
            for dr in range(8):
                grp = ((0xC1C0 if signed else 0xC0C0) | (dr << 9) | (m << 3)) >> 3
                def body(g, ea, op, signed=signed):
                    rs = rets(op)
                    if len(rs) != 1: raise Fail('mul rets')
                    ea_read(g, ea, 2, 'r4')
                    ld_dn(g, 'D', 'r5')
                    if signed: g.e('sexw\tr4,r4', 'sexw\tr5,r5')
                    else: g.e('extw\tr4,r4', 'extw\tr5,r5')
                    # the Leapster's MUL64 leaves the low word in MMID (as cc.sh)
                    g.e('mul64\t0,r5,r4', 'mov\tr0,mmid')
                    st_dn(g, 'D', 'r0')
                    g.e('mov\tr20,r0', 'and\tr22,r22,0x10')
                    # cycles: base + 2 * ones(src) (MULU), base + ones(src ^ src << 1) (MULS)
                    if signed: g.e('asl\tr6,r4,1', 'xor\tr6,r6,r4')
                    else: g.e('mov\tr6,r4')
                    popcount(g, 'r6')
                    # (FAME: MULS Dn counts the ones once, MULS <mem> and MULU twice)
                    if not signed or ea[0] != 0: g.e('asl\tr6,r6,1')
                    g.e('add\tr6,r6,%d' % rs[0])
                    g.nextr('r6')
                try_gen(grp, body, m)

def udiv(g, n, d, q, r):
    """q = n / d, r = n % d (32-bit unsigned, d != 0): restoring division with
    plain branches (no lp); clobbers r2, r3"""
    lp, sk = g.lab(), g.lab()
    g.e('mov\t%s,0' % r, 'mov\t%s,%s' % (q, n), 'mov\tr3,32')
    g.at(lp)
    g.e('lsr\tr2,%s,31' % q, 'asl\t%s,%s,1' % (q, q), 'asl\t%s,%s,1' % (r, r), 'or\t%s,%s,r2' % (r, r))
    g.e('cmp\t%s,%s' % (r, d), 'blo\t%df' % sk)
    g.e('sub\t%s,%s,%s' % (r, r, d), 'or\t%s,%s,1' % (q, q))
    g.at(sk)
    g.e('sub.f\tr3,r3,1', 'bne\t%db' % lp)

def f_divu():
    for m in range(8):
        if m == 1: continue
        for dr in range(8):
            grp = (0x80C0 | (dr << 9) | (m << 3)) >> 3
            def body(g, ea, op):
                rs = rets(op)       # [div by zero, overflow, normal (138 - 2 * ones(q))]
                if len(rs) != 3: raise Fail('divu rets %s' % rs)
                ea_read(g, ea, 2, 'r4')
                g.e('extw\tr4,r4')
                nz, nov = g.lab(), g.lab()
                g.e('brne\tr4,0,%df' % nz)
                g.e('sub\tr1,r14,r15', 'mov\tr0,5', 'bl\tk68_exc')
                g.next(rs[0])
                g.at(nz)
                ld_dn(g, 'D', 'r5')
                udiv(g, 'r5', 'r4', 'r6', 'r7')
                g.e('lsr.f\tr0,r6,16', 'beq\t%df' % nov)
                g.e('or\tr22,r22,2')       # overflow: V only (FAME keeps N, Z, C)
                g.next(rs[1])
                g.at(nov)
                g.e('asl\tr0,r7,16', 'or\tr0,r0,r6')
                st_dn(g, 'D', 'r0')
                g.e('asl\tr20,r6,16', 'and\tr22,r22,0x10')
                g.e('mov\tr0,r6'); popcount(g, 'r0')
                g.e('asl\tr0,r0,1', 'rsub\tr0,r0,%d' % rs[2])
                g.nextr('r0')
            try_gen(grp, body, m)

def f_misc_ops():
    for dr in range(8):          # EXG
        for opm, (a, b) in ((0x140, ('dn', 'dn')), (0x148, ('an', 'an')), (0x188, ('dn', 'an'))):
            grp = (0xC000 | (dr << 9) | opm) >> 3
            op0 = grp << 3
            if not legal(op0): continue
            g = G(op0)
            (ld_dn if b == 'dn' else ld_an)(g, 'S', 'r4')
            (ld_dn if a == 'dn' else ld_an)(g, 'D', 'r5')
            (st_dn if a == 'dn' else st_an)(g, 'D', 'r4')
            (st_dn if b == 'dn' else st_an)(g, 'S', 'r5')
            g.next(cyc1(op0)); put(grp, g)
    for dr in range(8):          # CMPM (Ay)+,(Ax)+
        for s in range(3):
            sz = (1, 2, 4)[s]
            grp = (0xB108 | (dr << 9) | (s << 6)) >> 3
            def body(g, ea, op, sz=sz):
                ea_addr(g, (3, 'S'), sz, 'r0'); rd(g, sz, 'r4')
                ea_addr(g, (3, 'D'), sz, 'r0'); rd(g, sz, 'r5')
                arith(g, 'cmp', 'r5', 'r4', sz)
                g.next(same_cyc(op, ea))
            try_gen(grp, body, 1)

FAMILIES_B = [f_shift, f_bitops, f_muldiv, f_divu, f_misc_ops]
