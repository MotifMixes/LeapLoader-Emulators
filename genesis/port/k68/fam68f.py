# fam68f.py: ABCD, SBCD, NBCD (FAME's formulas, famec_opcodes.h)
from ops68 import *

def nz_byte(g):
    """N = bit 7 of r0, Z cleared if the byte is nonzero, else kept (N = 0)"""
    n, e = g.lab(), g.lab()
    g.e('and\tr1,r0,0xff', 'breq\tr1,0,%df' % n, 'asl\tr20,r0,24', 'b\t%df' % e)
    g.at(n)
    g.e('cmp\tr20,0', 'mov.ne\tr20,1')
    g.at(e)

def bcd(g, kind, d, s):
    """d (+/-) s (+/-) X in BCD -> r0 (low byte), flags; d, s raw bytes (not r0-r3, r6, r7)"""
    g.e('lsr\tr3,r22,4', 'and\tr3,r3,1', 'and\tr1,%s,15' % d, 'and\tr2,%s,15' % s)
    if kind == 'add':
        g.e('add\tr0,r1,r2', 'add\tr0,r0,r3', 'mov\tr6,0', 'cmp\tr0,9', 'mov.hi\tr6,6',
            'and\tr1,%s,0xf0' % d, 'and\tr2,%s,0xf0' % s, 'add\tr0,r0,r1', 'add\tr0,r0,r2',
            'not\tr7,r0', 'add\tr0,r0,r6', 'mov\tr22,0')
        n = g.lab()
        g.e('cmp\tr0,0x9f', 'bls\t%df' % n, 'sub\tr0,r0,0xa0', 'mov\tr22,0x11')
        g.at(n)
        g.e('and\tr7,r7,r0')
    else:
        g.e('sub\tr0,r1,r2', 'sub\tr0,r0,r3', 'mov\tr6,0', 'cmp\tr0,15', 'mov.hi\tr6,6',
            'and\tr1,%s,0xf0' % d, 'and\tr2,%s,0xf0' % s, 'add\tr0,r0,r1', 'sub\tr0,r0,r2',
            'mov\tr7,r0', 'mov\tr22,0', 'cmp\tr0,r6', 'mov.lt\tr22,0x11')
        n = g.lab()
        g.e('cmp\tr0,0xff', 'bls\t%df' % n, 'add\tr0,r0,0xa0')
        g.at(n)
        g.e('sub\tr0,r0,r6', 'and\tr0,r0,0xff', 'bic\tr7,r7,r0')
    g.e('lsr\tr7,r7,6', 'and\tr7,r7,2', 'or\tr22,r22,r7')
    nz_byte(g)

def f_bcd():
    for top, kind in ((0xC100, 'add'), (0x8100, 'sub')):
        for dr in range(8):
            def body(g, ea, op, kind=kind):
                ld_dn(g, 'S', 'r4'); ld_dn(g, 'D', 'r5')
                bcd(g, kind, 'r5', 'r4'); st_dn(g, 'D', 'r0', 1); g.next(same_cyc(op, ea))
            try_gen((top | (dr << 9)) >> 3, body, 0)
            def body(g, ea, op, kind=kind):
                ea_addr(g, (4, 'S'), 1, 'r0'); rd(g, 1, 'r4')
                ea_addr(g, (4, 'D'), 1, 'r8'); g.e('mov\tr0,r8'); rd(g, 1, 'r5')
                bcd(g, kind, 'r5', 'r4'); wr(g, 1, 'r8', 'r0'); g.next(same_cyc(op, ea))
            try_gen((top | (dr << 9) | 8) >> 3, body, 1)
    for m in range(8):                 # NBCD
        if m == 1: continue
        def body(g, ea, op):
            c = same_cyc(op, ea)
            if ea[0] == 0: ld_dn(g, ea[1], 'r5')
            else: ea_addr(g, ea, 1, 'r8'); g.e('mov\tr0,r8'); rd(g, 1, 'r5')
            g.e('extb\tr5,r5', 'lsr\tr3,r22,4', 'and\tr3,r3,1', 'rsub\tr0,r5,0', 'sub\tr0,r0,r3')
            nz, n6, e = g.lab(), g.lab(), g.lab()
            g.e('brne\tr0,0,%df' % nz, 'mov\tr22,0', 'cmp\tr20,0', 'mov.ne\tr20,1', 'b\t%df' % e)
            g.at(nz)
            g.e('mov\tr7,r0', 'or\tr1,r0,r5', 'and.f\t0,r1,15', 'bne\t%df' % n6, 'and\tr0,r0,0xf0', 'add\tr0,r0,6')
            g.at(n6)
            g.e('add\tr0,r0,0x9a', 'and\tr0,r0,0xff', 'mov\tr4,r0')
            if ea[0] == 0: st_dn(g, ea[1], 'r4', 1)
            else: wr(g, 1, 'r8', 'r4')
            g.e('mov\tr0,r4', 'bic\tr7,r7,r0', 'lsr\tr7,r7,6', 'and\tr7,r7,2', 'or\tr22,r7,0x11')
            nz_byte(g)
            g.at(e)
            g.next(c)
        try_gen((0x4800 | (m << 3)) >> 3, body, m, (0, 1))

def set_n_only(g, src):
    """N = bit 31 of src; Z, V, C, X kept"""
    z, e = g.lab(), g.lab()
    g.e('lsr\tr1,%s,31' % src, 'bclr\tr22,r22,3', 'breq\tr20,0,%df' % z)
    g.e('asl\tr20,r1,31', 'b.d\t%df' % e, 'or\tr20,r20,1')
    g.at(z)
    g.e('asl\tr1,r1,3', 'or\tr22,r22,r1')
    g.at(e)

def f_chk():
    for dr in range(8):
        for m in range(8):
            if m == 1: continue
            def body(g, ea, op):        # CHK.W <ea>,Dn: Dn < 0 or > <ea>: N, exception 6
                rs = rets(op)
                if len(rs) != 2: raise Fail('chk rets %s' % rs)
                ea_read(g, ea, 2, 'r4'); ld_dn(g, 'D', 'r5')
                g.e('sexw\tr4,r4', 'sexw\tr5,r5')
                x, ok = g.lab(), g.lab()
                g.e('brlt\tr5,0,%df' % x, 'brle\tr5,r4,%df' % ok)
                g.at(x)
                set_n_only(g, 'r5')
                g.e('sub\tr1,r14,r15', 'mov\tr0,6', 'bl\tk68_exc')
                g.next(rs[0])
                g.at(ok)
                g.next(rs[1])
            try_gen((0x4180 | (dr << 9) | (m << 3)) >> 3, body, m)

FAMILIES_F = [f_bcd, f_chk]
