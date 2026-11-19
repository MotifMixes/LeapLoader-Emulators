# fam68d.py: DIVS (templates in ops68.py; FAME's cyclone-style DIV timing)
from ops68 import *
from fam68b import udiv, popcount

def divs_bases(op):
    """leading constants of FAME's five RETs: zero, 0x80000000/-1, big overflow,
    small overflow, normal"""
    body = '\n'.join(H[T[op]])
    v = [int(x) for x in re.findall(r'RET\((\d+)', body)]
    if len(v) != 5: raise Fail('divs rets %s' % v)
    return v

def f_divs():
    for m in range(8):
        if m == 1: continue
        for dr in range(8):
            grp = (0x81C0 | (dr << 9) | (m << 3)) >> 3
            def body(g, ea, op):
                z, mn, big, small, norm = divs_bases(op)
                ea_read(g, ea, 2, 'r4')
                g.e('sexw\tr4,r4')
                nz, nmn, ndo = g.lab(), g.lab(), g.lab()
                g.e('brne\tr4,0,%df' % nz)
                g.e('sub\tr1,r14,r15', 'mov\tr0,5', 'bl\tk68_exc')
                g.next(z)
                g.at(nz)
                ld_dn(g, 'D', 'r5')
                # 0x80000000 / -1: result 0, Z set, N V C clear
                g.e('cmp\tr5,0x80000000', 'bne\t%df' % ndo, 'cmp\tr4,-1', 'bne\t%df' % ndo)
                g.e('mov\tr0,0'); st_dn(g, 'D', 'r0')
                g.e('mov\tr20,0', 'and\tr22,r22,0x10')
                g.next(mn + 2)
                g.at(ndo)
                # magnitudes, unsigned divide, signs back (C: q to zero, r as dst)
                g.e('abs\tr8,r5', 'abs\tr9,r4')
                udiv(g, 'r8', 'r9', 'r6', 'r7')
                g.e('xor\tr0,r5,r4', 'cmp\tr0,0', 'rsub.lt\tr6,r6,0', 'cmp\tr5,0', 'rsub.lt\tr7,r7,0')
                g.e('lsr\tr10,r5,31')         # dst >> 31 (u32)
                nov, nsm = g.lab(), g.lab()
                # overflow: q > 0x7fff or q < -0x8000
                g.e('add\tr0,r6,0x8000', 'lsr.f\tr0,r0,16', 'beq\t%df' % nov)
                g.e('or\tr22,r22,2')          # V only
                g.e('add\tr0,r6,0x10000', 'lsr.f\tr0,r0,17', 'beq\t%df' % nsm)
                g.e('asl\tr0,r10,1', 'add\tr0,r0,%d' % big)
                g.nextr('r0')
                g.at(nsm)
                # small overflow: small + (dst>>31)*4 - (q>>31)*2 - 2*ones(abs(q)), q signed
                g.e('asr\tr11,r6,31', 'abs\tr0,r6'); popcount(g, 'r0')
                g.e('asl\tr0,r0,1', 'asl\tr1,r10,2', 'add\tr1,r1,%d' % small, 'sub\tr0,r1,r0',
                    'asl\tr11,r11,1', 'sub\tr0,r0,r11')
                g.nextr('r0')
                g.at(nov)
                g.e('extw\tr6,r6', 'asl\tr0,r7,16', 'or\tr0,r0,r6')
                st_dn(g, 'D', 'r0')
                g.e('asl\tr20,r6,16', 'and\tr22,r22,0x10')
                g.e('mov\tr0,r6'); popcount(g, 'r0')
                g.e('asl\tr0,r0,1', 'asl\tr1,r10,2', 'add\tr1,r1,%d' % norm, 'sub\tr0,r1,r0')
                g.nextr('r0')
            try_gen(grp, body, m)

FAMILIES_D = [f_divs]
