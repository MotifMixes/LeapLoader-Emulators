# fam68e.py: status register and system instructions of k68i.S: MOVE to/from
# SR/CCR, ANDI/ORI/EORI to CCR/SR, RTE, RTR, TRAP, TRAPV, MOVE USP, TAS, NOP/RTS
# (the 0x4E70 group). STOP, RESET, ILLEGAL, line A/F, CHK, BCD, MOVEP: FAME.
from ops68 import *

def priv(g):
    """FAME: not supervisor -> privilege violation (PC of the opcode), RET(0)"""
    n = g.lab()
    g.e('ld\tr0,[r13,0x80]', 'brne\tr0,0,%df' % n)
    g.e('sub\tr1,r14,r15', 'sub\tr1,r1,2', 'mov\tr0,8', 'bl\tk68_exc')
    g.next(0)
    g.at(n)

def intchk(g, clk):
    """FAME's CHECK_INT_TO_JUMP(clk)"""
    n = g.lab()
    g.e('ldb\tr0,[r13,0x48]', 'ld\tr1,[r13,0x84]', 'brls\tr0,r1,%df' % n)
    g.e('sub\tr2,r16,%d' % clk, 'st\tr2,[r13,0x5c]', 'mov\tr16,%d' % clk)
    g.at(n)

def pop(g, sz, d):
    g.e('ld\tr0,[r19,28]', 'add\tr1,r0,%d' % sz, 'st\tr1,[r19,28]'); rd(g, sz, d)

def f_sr():
    for m in range(8):
        if m == 1: continue
        def body(g, ea, op):        # MOVE SR,<ea>
            c = same_cyc(op, ea)
            if ea[0] == 0:
                g.e('bl\tk68_getsr'); st_dn(g, ea[1], 'r0', 2); g.next(c); return
            ea_addr(g, ea, 2, 'r6'); g.e('bl\tk68_getsr', 'mov\tr1,r0'); wr(g, 2, 'r6', 'r1'); g.next(c)
        try_gen((0x40C0 | (m << 3)) >> 3, body, m, (0, 1))
        def body(g, ea, op):        # MOVE <ea>,CCR
            c = same_cyc(op, ea)
            ea_read(g, ea, 2, 'r0'); g.e('bl\tk68_setccr'); g.next(c)
        try_gen((0x44C0 | (m << 3)) >> 3, body, m)
        def body(g, ea, op):        # MOVE <ea>,SR
            rs = rets(op)
            if len(rs) != 2 or rs[0] != 0: raise Fail('move sr rets %s' % rs)
            priv(g)
            ea_read(g, ea, 2, 'r0'); g.e('bl\tk68_setsr', 'bl\tk68_swapusp')
            intchk(g, rs[1]); g.next(rs[1])
        try_gen((0x46C0 | (m << 3)) >> 3, body, m)
    # ORI/ANDI/EORI to CCR (#byte) and SR (#word) = mode 7.4 of ORI/ANDI/EORI.B/W:
    # these groups again, with 7.0 / 7.1 (#imm,abs) as in f_imm
    for base, k in ((0x0000, 'or'), (0x0200, 'and'), (0x0A00, 'eor')):
        for s in (0, 1):
            sz = (1, 2)[s]
            grp = (base | 0x38 | (s << 6)) >> 3
            def body(g, ea, op, sz=sz, k=k):
                rs = rets(op)
                if ea == (7, 4):
                    x = {'eor': 'xor'}.get(k, k)
                    if sz == 1:
                        fetch_ext(g, 'r4', 1)
                        g.e('and\tr4,r4,0x1f', 'bl\tk68_getsr', 'and\tr0,r0,0x1f', '%s\tr0,r0,r4' % x, 'bl\tk68_setccr')
                        g.next(rs[0]); return
                    if len(rs) != 2: raise Fail('sr imm rets')
                    priv(g)
                    fetch_ext(g, 'r4', 2)
                    g.e('and\tr4,r4,0xa71f', 'bl\tk68_getsr', '%s\tr0,r0,r4' % x, 'bl\tk68_setsr', 'bl\tk68_swapusp')
                    intchk(g, rs[1]); g.next(rs[1]); return
                c = same_cyc(op, ea)
                fetch_ext(g, 'r5', sz)
                ea_addr(g, ea, sz, 'r6'); g.e('mov\tr0,r6'); rd(g, sz, 'r4')
                logic(g, k, 'r4', 'r5', sz, 'r1')
                wr(g, sz, 'r6', 'r1'); g.next(c)
            try_gen(grp, body, 7, (0, 1, 4))

def f_4e():
    # 0x4E70-0x4E77: RESET (FAME), NOP, STOP (FAME), RTE, (RTD: FAME), RTS, TRAPV, RTR
    g = G(0x4E70)
    labs = {r: g.lab() for r in (1, 3, 5, 6, 7)}
    g.e('and\tr0,r17,7')
    for r, n in labs.items(): g.e('cmp\tr0,%d' % r, 'beq\t%df' % n)
    g.e('b\tk68_fallback')
    g.at(labs[1]); g.next(cyc1(0x4E71))
    g.at(labs[5])
    pop(g, 4, 'r4'); set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(cyc1(0x4E75))
    g.at(labs[3])                    # RTE
    rs = rets(0x4E73)
    priv(g)
    pop(g, 2, 'r0'); g.e('bl\tk68_setsr')
    pop(g, 4, 'r4'); set_pc(g, 'r4'); g.e('bl\tk68_swapusp'); check_branch(g, 'r4')
    g.e('ldw\tr0,[r13,0x52]', 'bic\tr0,r0,0x1a', 'stw\tr0,[r13,0x52]')
    intchk(g, rs[1]); g.next(rs[1])
    g.at(labs[6])                    # TRAPV
    rs = rets(0x4E76)
    n = g.lab()
    if len(rs) != 2: raise Fail('trapv %s' % rs)
    g.e('bbit0\tr22,1,%df' % n, 'sub\tr1,r14,r15', 'mov\tr0,7', 'bl\tk68_exc')
    g.next(rs[0])
    g.at(n)
    g.next(rs[1])
    g.at(labs[7])                    # RTR
    pop(g, 2, 'r0'); g.e('bl\tk68_setccr')
    pop(g, 4, 'r4'); set_pc(g, 'r4'); check_branch(g, 'r4'); g.next(cyc1(0x4E77))
    put(0x4E70 >> 3, g)
    # mode-7 style template variants for the recompiler (no sub-dispatch)
    for grp, base in ((0x4E40 >> 3, 32), (0x4E48 >> 3, 40)):   # TRAP #n
        g = G(grp << 3)
        g.e('and\tr0,r17,7', 'add\tr0,r0,%d' % base, 'sub\tr1,r14,r15', 'bl\tk68_exc')
        g.next(rets(grp << 3)[0]); put(grp, g)
    g = G(0x4E60)                    # MOVE An,USP
    priv(g); ld_an(g, 'S', 'r0'); g.e('st\tr0,[r13,0x40]'); g.next(rets(0x4E60)[-1]); put(0x4E60 >> 3, g)
    g = G(0x4E68)                    # MOVE USP,An
    priv(g); g.e('ld\tr0,[r13,0x40]'); st_an(g, 'S', 'r0'); g.next(rets(0x4E68)[-1]); put(0x4E68 >> 3, g)

def f_tas():
    for m in range(8):
        if m == 1: continue
        def body(g, ea, op):         # TAS (main 68k: no write back to memory, as FAME)
            c = same_cyc(op, ea)
            if ea[0] == 0:
                ld_dn(g, ea[1], 'r4'); flags_logic(g, 'r4', 1)
                g.e('or\tr4,r4,0x80'); st_dn(g, ea[1], 'r4', 1); g.next(c); return
            ea_read(g, ea, 1, 'r4'); flags_logic(g, 'r4', 1); g.next(c)
        try_gen((0x4AC0 | (m << 3)) >> 3, body, m, (0, 1))

FAMILIES_E = [f_sr, f_4e, f_tas]
