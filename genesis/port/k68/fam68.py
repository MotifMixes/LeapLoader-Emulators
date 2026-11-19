# fam68.py: the 68000 instruction families of k68i.S (templates in ops68.py)
from ops68 import *
import ops68

def f_move():
    for grp in range(0x1000 >> 3, 0x4000 >> 3):
        op0 = grp << 3
        sz = {1: 1, 3: 2, 2: 4}[op0 >> 12]
        dm, dr, sm = (op0 >> 6) & 7, (op0 >> 9) & 7, (op0 >> 3) & 7
        if dm == 7 and dr > 1: continue
        def body(g, ea, op, sz=sz, dm=dm, dr=dr):
            if dm == 1:     # MOVEA
                if sz == 1: raise Fail('movea.b')
                ea_read(g, ea, sz, 'r4')
                if sz == 2: g.e('sexw\tr4,r4')
                st_an(g, 'D', 'r4')
                g.next(same_cyc(op, ea)); return
            ea_read(g, ea, sz, 'r4')
            flags_logic(g, 'r4', sz)
            g.dec_long = (sz == 4 and dm == 4)
            ea_write(g, (dm, dr if dm == 7 else 'D'), sz, 'r4')
            g.next(same_cyc(op, ea))
        try_gen(grp, body, sm)

def f_moveq():
    for grp in range(0x7000 >> 3, 0x8000 >> 3):
        op0 = grp << 3
        if op0 & 0x100: continue
        dr = (op0 >> 9) & 7
        g = G(op0)
        try:
            c = same_cyc(op0, (0, 'S'))
        except Fail: continue
        g.e('sexb\tr0,r17')      # data: bits 0-7
        st_dn(g, 'D', 'r0')
        g.e('mov\tr20,r0', 'and\tr22,r22,0x10')
        g.next(c)
        put(grp, g)

IDLE_GRPS = {0x66f0 >> 3, 0x66f8 >> 3, 0x67f0 >> 3, 0x67f8 >> 3, 0x60f8 >> 3}
IDLE_OPS = {0x66fa, 0x66f8, 0x66f6, 0x66f2, 0x67fa, 0x67f8, 0x67f6, 0x67f2, 0x60fe, 0x60fc}

def f_bcc():
    for grp in range(0x6000 >> 3, 0x7000 >> 3):
        op0 = grp << 3
        cc = (op0 >> 8) & 15
        g = G(op0)
        if grp in IDLE_GRPS:     # FAME's idle loop detector (when installed)
            n = g.lab()
            g.e('ld\tr0,[k68_idle_on]', 'breq\tr0,0,%df' % n)
            if IDLE2:    # -DK68IDLE2: FAME only while its table entry is still the detector
                m = g.lab()
                for o in sorted(o for o in IDLE_OPS if o >> 3 == grp):
                    g.e('cmp\tr17,%d' % o, 'beq\t%df' % m)
                g.e('b\t%df' % n)
                g.at(m)
                g.e('ld\tr0,[k68_fametab]', 'ld.as\tr0,[r0,r17]', 'ld\tr1,[k68_detfn]', 'cmp\tr0,r1', 'beq\tk68_fallback')
            else:
                for o in sorted(o for o in IDLE_OPS if o >> 3 == grp):
                    g.e('cmp\tr17,%d' % o, 'beq\tk68_fallback')
            g.at(n)
        if (op0 & 0xf8) == 0:
            nw = g.lab()
            g.e('and\tr0,r17,7', 'breq\tr0,0,%df' % nw)
            short_br(g, cc, op0 | 1)
            g.at(nw)
            word_br(g, cc, op0)
        else:
            short_br(g, cc, op0 | 1)
        put(grp, g)

import os
IDLE2 = bool(os.environ.get('K68IDLE2'))
POLL = bool(os.environ.get('K68POLL'))
# FAME's "not idle" patched branches (idle_detector_bcc8 newop | 0x200): bne 0x73xx, beq 0x77xx,
# bra 0x7fxx with the original displacement; FAME runs them as plain Bcc while idle detection is on
FAKE_OPS = {0x73fa: 6, 0x73f8: 6, 0x73f6: 6, 0x73f2: 6, 0x77fa: 7, 0x77f8: 7, 0x77f6: 7, 0x77f2: 7, 0x7ffe: 0, 0x7ffc: 0}
def f_idlefake():
    if not IDLE2: return
    for grp in sorted(set(o >> 3 for o in FAKE_OPS)):
        ops = sorted(o for o in FAKE_OPS if o >> 3 == grp)
        cc = FAKE_OPS[ops[0]]
        g = G(grp << 3)
        n = g.lab()
        g.e('ld\tr0,[k68_idle_on]', 'tst\tr0,r0', 'beq\tk68_fallback')
        for o in ops[:-1]:
            g.e('cmp\tr17,%d' % o, 'beq\t%df' % n)
        g.e('cmp\tr17,%d' % ops[-1], 'bne\tk68_fallback')
        g.at(n)
        if POLL and (grp << 3) in (0x73f0, 0x77f0):   # -DK68POLL: 73f6/77f6 closing a btst abs.l loop -> k68_pollbcc
            m = g.lab()
            g.e('cmp\tr17,%d' % (grp << 3 | 6), 'bne\t%df' % m, 'ldw\tr0,[r14,-10]', 'cmp\tr0,0x0839', 'beq\tk68_fallback')
            g.at(m)
        short_br(g, cc, ((0x60 | cc) << 8) | (ops[0] & 0xff) | 1)
        put(grp, g)

def short_br(g, cc, op):
    rs = rets(op)
    if cc == 0:      # BRA.s: rebase, odd displacement -> address error
        g.e('sub\tr4,r14,r15', 'sexb\tr0,r17', 'add\tr4,r4,r0')
        set_pc(g, 'r4'); g.e('btst\tr17,0', 'bne\tk68_addrerr'); g.next(rs[0])
    elif cc == 1:    # BSR.s
        g.e('sub\tr4,r14,r15'); push32(g, 'r4')
        g.e('sexb\tr0,r17', 'add\tr4,r4,r0')
        set_pc(g, 'r4'); g.e('btst\tr17,0', 'bne\tk68_addrerr'); g.next(rs[0])
    else:            # Bcc.s: PC += disp & ~1, no rebase; taken: 2 more cycles
        n = g.lab()
        cond(g, cc, '%df' % n, neg=True)
        g.e('sexb\tr0,r17', 'bclr\tr0,r0,0', 'add\tr14,r14,r0')
        g.next(rs[0] + 2)
        g.at(n)
        g.next(rs[0])

def word_br(g, cc, op):
    rs = rets(op)
    if cc == 0:
        g.e('ldw.x\tr0,[r14,0]', 'sub\tr4,r14,r15', 'add\tr4,r4,r0')
        set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(rs[0])
    elif cc == 1:
        g.e('sub\tr4,r14,r15', 'add\tr5,r4,2'); push32(g, 'r5')
        g.e('ldw.x\tr0,[r14,0]', 'add\tr4,r4,r0')
        set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(rs[0])
    else:
        n = g.lab()
        cond(g, cc, '%df' % n, neg=True)
        g.e('ldw.x\tr0,[r14,0]', 'sub\tr4,r14,r15', 'add\tr4,r4,r0')
        set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(rs[0])
        g.at(n)
        g.e('add\tr14,r14,2')
        g.next(rs[1])

def f_dbcc_scc():
    for cc in range(16):
        for m in range(8):
            grp = (0x50C0 | (cc << 8) | (m << 3)) >> 3
            op0 = grp << 3
            if m == 1:     # DBcc
                rs = rets(op0)
                g = G(op0)
                if cc == 0:          # DBT: never loops
                    g.e('add\tr14,r14,2'); g.next(rs[0]); put(grp, g); continue
                g.e('mov\tr0,1', 'stb\tr0,[r13,0x88]')     # NOT_POLLING
                nt, nx = g.lab(), g.lab()
                if cc != 1:
                    cond(g, cc, '%df' % nt)
                ld_dn(g, 'S', 'r4')
                g.e('sub\tr4,r4,1')
                g.e('add2\tr3,r13,r3', 'stw\tr4,[r3]')
                g.e('bmsk\tr4,r4,15', 'breq\tr4,0xffff,%df' % nx)
                g.e('ldw.x\tr0,[r14,0]', 'sub\tr4,r14,r15', 'add\tr4,r4,r0')
                set_pc(g, 'r4'); check_branch(g, 'r4')
                if cc == 1:
                    g.next(rs[0]); g.at(nx); g.e('add\tr14,r14,2'); g.next(rs[1])
                else:      # FAME: [10 taken, 12 cond true, 14 expired]
                    g.next(rs[0]); g.at(nt); g.e('add\tr14,r14,2'); g.next(rs[1])
                    g.at(nx); g.e('add\tr14,r14,2'); g.next(rs[2])
                put(grp, g)
                continue
            def body(g, ea, op, cc=cc):
                rs = rets(op)
                if ea[0] == 0:
                    n = g.lab()
                    g.e('mov\tr4,0'); cond(g, cc, '%df' % n, neg=True); g.e('mov\tr4,0xff')
                    g.at(n)
                    st_dn(g, ea[1], 'r4', 1)
                    if len(rs) == 1: g.next(rs[0]); return
                    n2 = g.lab()      # [true, false]
                    g.e('breq\tr4,0,%df' % n2); g.next(rs[0]); g.at(n2); g.next(rs[1])
                    return
                ea_addr(g, ea, 1, 'r5')
                n = g.lab()
                g.e('mov\tr4,0'); cond(g, cc, '%df' % n, neg=True); g.e('mov\tr4,0xff')
                g.at(n)
                if len(set(rs)) != 1: raise Fail('scc cycles')
                wr(g, 1, 'r5', 'r4'); g.next(rs[0])
            try_gen(grp, body, m, (0, 1))

def f_quick():
    for grp in range(0x5000 >> 3, 0x6000 >> 3):
        op0 = grp << 3
        s = (op0 >> 6) & 3
        if s == 3: continue
        sz = (1, 2, 4)[s]
        q = (op0 >> 9) & 7 or 8
        kind = 'sub' if op0 & 0x100 else 'add'
        m = (op0 >> 3) & 7
        def body(g, ea, op, sz=sz, q=q, kind=kind):
            c = same_cyc(op, ea)
            if ea[0] == 1:    # An: whole register, no flags
                ld_an(g, ea[1], 'r4')
                g.e('%s\tr4,r4,%d' % (kind, q))
                st_an(g, ea[1], 'r4'); g.next(c); return
            g.e('mov\tr5,%d' % q)
            if ea[0] == 0:
                ld_dn(g, ea[1], 'r4')
                arith(g, kind, 'r4', 'r5', sz, 'r0')
                st_dn(g, ea[1], 'r0', sz); g.next(c); return
            ea_addr(g, ea, sz, 'r6'); g.e('mov\tr0,r6'); rd(g, sz, 'r4')
            arith(g, kind, 'r4', 'r5', sz, 'r1')
            wr(g, sz, 'r6', 'r1'); g.next(c)
        try_gen(grp, body, m, (0, 1))

def f_imm():
    kinds = {0x0000: 'or', 0x0200: 'and', 0x0400: 'sub', 0x0600: 'add', 0x0A00: 'eor', 0x0C00: 'cmp'}
    for base, kind in kinds.items():
        for s in range(3):
            sz = (1, 2, 4)[s]
            for m in range(8):
                grp = (base | (s << 6) | (m << 3)) >> 3
                if m == 1: continue
                def body(g, ea, op, sz=sz, kind=kind):
                    c = same_cyc(op, ea)
                    fetch_ext(g, 'r5', sz)
                    if ea[0] == 0:
                        ld_dn(g, ea[1], 'r4')
                        if kind in ('and', 'or', 'eor'): logic(g, kind, 'r4', 'r5', sz)
                        else: arith(g, kind, 'r4', 'r5', sz)
                        if kind != 'cmp': st_dn(g, ea[1], 'r0', sz)
                        g.next(c); return
                    ea_addr(g, ea, sz, 'r6'); g.e('mov\tr0,r6'); rd(g, sz, 'r4')
                    if kind in ('and', 'or', 'eor'): logic(g, kind, 'r4', 'r5', sz, 'r1')
                    else: arith(g, kind, 'r4', 'r5', sz, 'r1')
                    if kind != 'cmp': wr(g, sz, 'r6', 'r1')
                    g.next(c)
                try_gen(grp, body, m, (0, 1, 2, 3) if kind == 'cmp' else (0, 1))

def f_alu():
    """OR/SUB/CMP/EOR/AND/ADD with a data register; ADDA/SUBA/CMPA"""
    for top, kind in ((0x8000, 'or'), (0x9000, 'sub'), (0xB000, 'cmp'), (0xC000, 'and'), (0xD000, 'add')):
        for dr in range(8):
            for om in range(8):
                for m in range(8):
                    grp = (top | (dr << 9) | (om << 6) | (m << 3)) >> 3
                    if om in (3, 7):
                        if kind in ('or', 'and'): continue      # DIV/MUL
                        sz = 2 if om == 3 else 4
                        def body(g, ea, op, sz=sz, dr=dr, kind=kind):
                            c = same_cyc(op, ea)
                            ea_read(g, ea, sz, 'r4')
                            if sz == 2: g.e('sexw\tr4,r4')
                            ld_an(g, 'D', 'r5')
                            if kind == 'cmp': arith(g, 'cmp', 'r5', 'r4', 4)
                            else:
                                g.e('%s\tr5,r5,r4' % kind); st_an(g, 'D', 'r5')
                            g.next(c)
                        try_gen(grp, body, m)
                        continue
                    sz = (1, 2, 4)[om & 3]
                    if om < 4:      # <ea> op Dn -> Dn
                        if m == 1 and (sz == 1 or kind in ('or', 'and')): continue
                        def body(g, ea, op, sz=sz, dr=dr, kind=kind):
                            c = same_cyc(op, ea)
                            ea_read(g, ea, sz, 'r4')
                            ld_dn(g, 'D', 'r5')
                            if kind in ('or', 'and'): logic(g, kind, 'r5', 'r4', sz)
                            else: arith(g, kind, 'r5', 'r4', sz)
                            if kind != 'cmp': st_dn(g, 'D', 'r0', sz)
                            g.next(c)
                        try_gen(grp, body, m)
                    else:           # Dn op <ea> -> <ea> (cmp: EOR)
                        k = 'eor' if kind == 'cmp' else kind
                        if m == 1 or (m == 0 and k != 'eor'): continue  # ADDX/SUBX/ABCD/SBCD/EXG/CMPM
                        def body(g, ea, op, sz=sz, dr=dr, k=k):
                            c = same_cyc(op, ea)
                            if ea[0] == 0:
                                ld_dn(g, ea[1], 'r4'); ld_dn(g, 'D', 'r5')
                                logic(g, k, 'r4', 'r5', sz)
                                st_dn(g, ea[1], 'r0', sz); g.next(c); return
                            ea_addr(g, ea, sz, 'r6'); g.e('mov\tr0,r6'); rd(g, sz, 'r4')
                            ld_dn(g, 'D', 'r5')
                            if k in ('or', 'and', 'eor'): logic(g, k, 'r4', 'r5', sz, 'r1')
                            else: arith(g, k, 'r4', 'r5', sz, 'r1')
                            wr(g, sz, 'r6', 'r1'); g.next(c)
                        try_gen(grp, body, m, (0, 1))

def f_unary():
    """CLR, NEG, NOT, TST"""
    for base, kind in ((0x4200, 'clr'), (0x4400, 'neg'), (0x4600, 'not'), (0x4A00, 'tst')):
        for s in range(3):
            sz = (1, 2, 4)[s]
            for m in range(8):
                if m == 1: continue
                grp = (base | (s << 6) | (m << 3)) >> 3
                def body(g, ea, op, sz=sz, kind=kind):
                    c = same_cyc(op, ea)
                    if kind == 'clr':
                        g.e('mov\tr4,0', 'mov\tr20,0', 'and\tr22,r22,0x10')
                        ea_write(g, ea, sz, 'r4'); g.next(c); return
                    if ea[0] == 0: ld_dn(g, ea[1], 'r4')
                    elif kind == 'tst': ea_read(g, ea, sz, 'r4')
                    else: ea_addr(g, ea, sz, 'r6'); g.e('mov\tr0,r6'); rd(g, sz, 'r4')
                    if kind == 'tst': flags_logic(g, 'r4', sz); g.next(c); return
                    if kind == 'not': g.e('not\tr1,r4'); flags_logic(g, 'r1', sz)
                    else: g.e('mov\tr5,0'); arith(g, 'sub', 'r5', 'r4', sz, 'r1')
                    if ea[0] == 0: st_dn(g, ea[1], 'r1', sz)
                    else: wr(g, sz, 'r6', 'r1')
                    g.next(c)
                try_gen(grp, body, m, (0, 1, 2, 3, 4) if kind == 'tst' else (0, 1))

def f_lea_pea_jmp():
    for dr in range(8):
        for m in (2, 5, 6, 7):
            grp = (0x41C0 | (dr << 9) | (m << 3)) >> 3
            def body(g, ea, op, dr=dr):
                ea_addr(g, ea, 4, 'r4'); st_an(g, 'D', 'r4'); g.next(same_cyc(op, ea))
            try_gen(grp, body, m, (0, 1, 2, 3))
    for m in (2, 5, 6, 7):
        def body(g, ea, op):        # PEA
            ea_addr(g, ea, 4, 'r4'); push32(g, 'r4'); g.next(same_cyc(op, ea))
        try_gen((0x4840 | (m << 3)) >> 3, body, m, (0, 1, 2, 3))
        def body(g, ea, op):        # JSR
            ea_addr(g, ea, 4, 'r4'); g.e('sub\tr5,r14,r15'); push32(g, 'r5')
            set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(same_cyc(op, ea))
        try_gen((0x4E80 | (m << 3)) >> 3, body, m, (0, 1, 2, 3))
        def body(g, ea, op):        # JMP
            ea_addr(g, ea, 4, 'r4'); set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(same_cyc(op, ea))
        try_gen((0x4EC0 | (m << 3)) >> 3, body, m, (0, 1, 2, 3))

def f_misc4():
    def body(g, ea, op):     # SWAP
        ld_dn(g, 'S', 'r4'); g.e('swap\tr4,r4'); st_dn(g, 'S', 'r4'); flags_logic(g, 'r4', 4); g.next(same_cyc(op, ea))
    try_gen(0x4840 >> 3, body, 0)
    def body(g, ea, op):     # EXT.W
        ld_dn(g, 'S', 'r4'); g.e('sexb\tr4,r4'); st_dn(g, 'S', 'r4', 2); flags_logic(g, 'r4', 2); g.next(same_cyc(op, ea))
    try_gen(0x4880 >> 3, body, 0)
    def body(g, ea, op):     # EXT.L
        ld_dn(g, 'S', 'r4'); g.e('sexw\tr4,r4'); st_dn(g, 'S', 'r4'); flags_logic(g, 'r4', 4); g.next(same_cyc(op, ea))
    try_gen(0x48C0 >> 3, body, 0)
    # 0x4E70-0x4E77: NOP, RTS here (the others: FAME)
    g = G(0x4E70)
    n1, n5 = g.lab(), g.lab()
    g.e('and\tr0,r17,7', 'cmp\tr0,1', 'beq\t%df' % n1, 'cmp\tr0,5', 'beq\t%df' % n5, 'b\tk68_fallback')
    g.at(n1); g.next(cyc1(0x4E71))
    g.at(n5)
    g.e('ld\tr0,[r19,28]', 'add\tr1,r0,4', 'st\tr1,[r19,28]'); rd(g, 4, 'r4')
    set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(cyc1(0x4E75))
    put(0x4E70 >> 3, g)
    # LINK An (A7: FAME), UNLK An
    g = G(0x4E50)
    g.e('and\tr0,r17,7', 'cmp\tr0,7', 'beq\tk68_fallback')
    ld_an(g, 'S', 'r4'); push32(g, 'r4')
    g.e('ld\tr4,[r19,28]'); st_an(g, 'S', 'r4')
    g.e('ldw.x\tr0,[r14,0]', 'add\tr14,r14,2', 'add\tr4,r4,r0', 'st\tr4,[r19,28]')
    g.next(cyc1(0x4E50)); put(0x4E50 >> 3, g)
    g = G(0x4E58)
    ld_an(g, 'S', 'r0'); g.e('add\tr1,r0,4', 'st\tr1,[r19,28]'); rd(g, 4, 'r4'); st_an(g, 'S', 'r4')
    g.next(cyc1(0x4E58)); put(0x4E58 >> 3, g)

def f_movem():
    for to_reg in (0, 1):
        for s in (0, 1):
            sz = 4 if s else 2
            for m in range(2, 8):
                grp = ((0x4C80 if to_reg else 0x4880) | (s << 6) | (m << 3)) >> 3
                if to_reg and m == 4: continue
                if not to_reg and m == 3: continue
                def body(g, ea, op, sz=sz, to_reg=to_reg):
                    rs = rets(op)
                    if len(rs) != 1: raise Fail('movem rets')
                    fetch_ext(g, 'r5', 2)        # register mask
                    if ea[0] == 4:               # regs -> -(An): A7 first, downwards
                        ld_an(g, ea[1], 'r4'); g.e('mov\tr7,r4', 'add\tr6,r13,60')
                        lp, sk, en = g.lab(), g.lab(), g.lab()
                        g.e('breq\tr5,0,%df' % en)
                        g.at(lp)
                        g.e('bbit0\tr5,0,%df' % sk, 'sub\tr4,r4,%d' % sz, 'ld\tr1,[r6,0]', 'mov\tr0,r4',
                            'bl\tk68_wr%s' % ('32d' if sz == 4 else '16'))
                        g.at(sk)
                        g.e('lsr.f\tr5,r5', 'sub\tr6,r6,4', 'bne\t%db' % lp)
                        g.at(en)
                        st_an(g, ea[1], 'r4')
                        g.e('sub\tr0,r7,r4', 'asl\tr0,r0,1', 'add\tr0,r0,%d' % rs[0])
                        g.nextr('r0'); return
                    if ea[0] == 3: ld_an(g, ea[1], 'r4')
                    else: ea_addr(g, ea, sz, 'r4')
                    g.e('mov\tr7,r4', 'mov\tr6,r13')
                    lp, sk, en = g.lab(), g.lab(), g.lab()
                    g.e('breq\tr5,0,%df' % en)
                    g.at(lp)
                    g.e('bbit0\tr5,0,%df' % sk, 'mov\tr0,r4')
                    if to_reg:
                        g.e('bl\tk68_rd%d' % (sz * 8))
                        if sz == 2: g.e('sexw\tr0,r0')
                        g.e('st\tr0,[r6,0]')
                    else:
                        g.e('ld\tr1,[r6,0]', 'bl\tk68_wr%d' % (sz * 8))
                    g.e('add\tr4,r4,%d' % sz)
                    g.at(sk)
                    g.e('lsr.f\tr5,r5', 'add\tr6,r6,4', 'bne\t%db' % lp)
                    g.at(en)
                    if ea[0] == 3: st_an(g, ea[1], 'r4')
                    g.e('sub\tr0,r4,r7', 'asl\tr0,r0,1', 'add\tr0,r0,%d' % rs[0])
                    g.nextr('r0')
                try_gen(grp, body, m, (0, 1, 2, 3) if to_reg else (0, 1))

FAMILIES = [f_move, f_moveq, f_bcc, f_idlefake, f_dbcc_scc, f_quick, f_imm, f_alu, f_unary, f_lea_pea_jmp,
            f_misc4, f_movem]

def gen(tab):
    import fam68b, fam68c, fam68d, fam68e, fam68f
    ops68.table = tab
    for f in FAMILIES + fam68b.FAMILIES_B + fam68c.FAMILIES_C + fam68d.FAMILIES_D + fam68e.FAMILIES_E + fam68f.FAMILIES_F:
        f()
    return ops68.handlers, ops68.fails
