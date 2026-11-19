# fam68c.py: ADDX/SUBX/NEGX, ROXL/ROXR, BTST #imm source (templates in ops68.py)
from ops68 import *

def xarith(g, kind, d, s, sz):
    """d + s + X (kind 'add') or d - s - X ('sub'); Z only cleared. d, s raw
    (not r0-r3, r6, r7); result (low bytes) -> r0"""
    S = sh(sz)
    if S: g.e('asl\tr1,%s,%d' % (s, S), 'asl\tr2,%s,%d' % (d, S))
    else: g.e('mov\tr1,%s' % s, 'mov\tr2,%s' % d)
    g.e('lsr\tr3,r22,4', 'and\tr3,r3,1')
    if S: g.e('asl\tr3,r3,%d' % S)
    g.e('mov\tr6,0', '%s.f\tr0,r2,r1' % kind, 'mov.cs\tr6,1', '%s.f\tr0,r0,r3' % kind, 'mov.cs\tr6,1')
    if kind == 'add': g.e('xor\tr7,r1,r0', 'xor\tr3,r2,r0', 'and\tr7,r7,r3')
    else: g.e('xor\tr7,r1,r2', 'xor\tr3,r0,r2', 'and\tr7,r7,r3')
    g.e('lsr\tr7,r7,31', 'asl\tr7,r7,1', 'or\tr6,r6,r7', 'asl\tr3,r6,4', 'and\tr3,r3,0x10', 'or\tr22,r6,r3')
    n, e = g.lab(), g.lab()
    g.e('breq\tr0,0,%df' % n, 'mov\tr20,r0', 'b\t%df' % e)
    g.at(n)
    g.e('cmp\tr20,0', 'mov.ne\tr20,1')       # result 0: Z unchanged, N = 0
    g.at(e)
    if S: g.e('lsr\tr0,r0,%d' % S)

def f_x():
    for top, kind in ((0xD100, 'add'), (0x9100, 'sub')):
        for dr in range(8):
            for s in range(3):
                sz = (1, 2, 4)[s]
                grp = (top | (dr << 9) | (s << 6)) >> 3            # Dy,Dx
                def body(g, ea, op, sz=sz, kind=kind):
                    ld_dn(g, 'S', 'r4'); ld_dn(g, 'D', 'r5')
                    xarith(g, kind, 'r5', 'r4', sz)
                    st_dn(g, 'D', 'r0', sz); g.next(same_cyc(op, ea))
                try_gen(grp, body, 0)
                grp = (top | (dr << 9) | (s << 6) | 8) >> 3        # -(Ay),-(Ax)
                def body(g, ea, op, sz=sz, kind=kind):
                    ea_addr(g, (4, 'S'), sz, 'r0'); rd(g, sz, 'r4')
                    ea_addr(g, (4, 'D'), sz, 'r8'); g.e('mov\tr0,r8'); rd(g, sz, 'r5')
                    xarith(g, kind, 'r5', 'r4', sz)
                    wr(g, sz, 'r8', 'r0', dec=False); g.next(same_cyc(op, ea))
                try_gen(grp, body, 1)
    for s in range(3):                                              # NEGX
        sz = (1, 2, 4)[s]
        for m in range(8):
            if m == 1: continue
            grp = (0x4000 | (s << 6) | (m << 3)) >> 3
            def body(g, ea, op, sz=sz):
                c = same_cyc(op, ea)
                g.e('mov\tr5,0')
                if ea[0] == 0:
                    ld_dn(g, ea[1], 'r4'); xarith(g, 'sub', 'r5', 'r4', sz)
                    st_dn(g, ea[1], 'r0', sz); g.next(c); return
                ea_addr(g, ea, sz, 'r8'); g.e('mov\tr0,r8'); rd(g, sz, 'r4')
                xarith(g, 'sub', 'r5', 'r4', sz)
                wr(g, sz, 'r8', 'r0'); g.next(c)
            try_gen(grp, body, m, (0, 1))

def rox_body(g, left, sz, c):
    """(N+1)-bit rotate through X. value r4, c: int 1-8 or 'r5' (1-63, reduced
    mod N+1 here). Result -> r0, C = X = last bit out, V = 0."""
    N = 8 * sz; S = 32 - N
    if sz < 4: g.e('%s\tr4,r4' % ('extb' if sz == 1 else 'extw'))
    g.e('lsr\tr6,r22,4', 'and\tr6,r6,1')        # X
    dyn = not isinstance(c, int)
    if dyn:
        # k = c mod (N+1) by subtraction (no divide): c <= 63
        lp, en = g.lab(), g.lab()
        g.e('mov\tr7,r5')
        g.at(lp)
        g.e('cmp\tr7,%d' % (N + 1), 'blo\t%df' % en, 'sub\tr7,r7,%d' % (N + 1), 'b\t%db' % lp)
        g.at(en)
        k = 'r7'
    else:
        k = c
    # 64-bit view {X, value}: do it bit by bit for k steps (k <= 32)
    lp, en = g.lab(), g.lab()
    if dyn: g.e('mov\tr3,r7')
    else: g.e('mov\tr3,%d' % k)
    g.e('breq\tr3,0,%df' % en)
    g.at(lp)
    if left:
        g.e('lsr\tr2,r4,%d' % (N - 1), 'and\tr2,r2,1', 'asl\tr4,r4,1', 'or\tr4,r4,r6', 'mov\tr6,r2')
    else:
        g.e('and\tr2,r4,1', 'lsr\tr4,r4,1')
        g.e('asl\tr1,r6,%d' % (N - 1), 'or\tr4,r4,r1', 'mov\tr6,r2')
    g.e('sub.f\tr3,r3,1', 'bne\t%db' % lp)
    g.at(en)
    if sz < 4: g.e('%s\tr4,r4' % ('extb' if sz == 1 else 'extw'))
    g.e('mov\tr0,r4', ('asl\tr20,r0,%d' % S) if S else 'mov\tr20,r0')
    g.e('asl\tr22,r6,4', 'or\tr22,r22,r6')     # X = C = last bit out (k = 0: C = X)

def f_rox():
    for cnt in range(8):
        for left in (0, 1):
            for s in range(3):
                sz = (1, 2, 4)[s]
                for ir in (0, 1):
                    grp = (0xE010 | (cnt << 9) | (left << 8) | (s << 6) | (ir << 5)) >> 3
                    op0 = grp << 3
                    rs = rets(op0)
                    if not legal(op0) or len(set(rs)) != 1:
                        fails.append('%04x: rox rets %s' % (op0, rs)); continue
                    g = G(op0)
                    ld_dn(g, 'S', 'r4')
                    if ir == 0:
                        c = cnt or 8
                        rox_body(g, left, sz, c)
                        st_dn(g, 'S', 'r0', sz)
                        g.next(rs[0] + 2 * c)
                    else:
                        nz = g.lab()
                        g.e('ld\tr5,[r13,%d]' % (cnt * 4), 'and.f\tr5,r5,63', 'bne\t%df' % nz)
                        # count 0: C = X, N Z from the operand, V = 0
                        flags_logic(g, 'r4', sz)
                        g.e('lsr\tr0,r22,4', 'or\tr22,r22,r0')
                        g.next(rs[0])
                        g.at(nz)
                        rox_body(g, left, sz, 'r5')
                        st_dn(g, 'S', 'r0', sz)
                        g.e('asl\tr0,r5,1', 'add\tr0,r0,%d' % rs[0])
                        g.nextr('r0')
                    put(grp, g)
    for left in (0, 1):            # memory, by 1
        for m in range(2, 8):
            grp = (0xE4C0 | (left << 8) | (m << 3)) >> 3
            def body(g, ea, op, left=left):
                c = same_cyc(op, ea)
                ea_addr(g, ea, 2, 'r8'); g.e('mov\tr0,r8'); rd(g, 2, 'r4')
                rox_body(g, left, 2, 1)
                wr(g, 2, 'r8', 'r0'); g.next(c)
            try_gen(grp, body, m, (0, 1))

FAMILIES_C = [f_x, f_rox]
