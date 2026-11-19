; ymx.S (-DYMASM): PicoDrive's YM2612 chan_render_loop for the common case
; (mono, output enabled, no DAC bit drop; ymloop.c checks and calls
; ymx_loop(ct, buffer, length)); same results as the C loop.
; Operator: s = phase index (10 bits used); p = E + ymx_sin[s & 0x1ff];
; out = ymx_tl[min(p, 3328)] (ymx_tl[3328] = 0), negated if s & 0x200.
; E = min((vol_out & ~1) << 2, 3328), kept by C in ct->vol_out1..4 (u16);
; a silent operator (vol_out >= ENV_QUIET) gives 0, as PicoDrive's skip does.
; Envelope steps: C (ymx_eg_run) with phases and eg_timer stored in ct.
; No lp, no gp.
	.text

#define C_E1	0	/* u16 x4 E1..E4 (in place of vol_out1..4) */
#define C_JMP	0x0c	/* algo routine (in place of pad) */
#define C_PH1	0x10
#define C_INC1	0x20
#define C_LFOCNT	0x30
#define C_LFOINC	0x34
#define C_MEM	0x38
#define C_EGCNT	0x3c
#define C_EGT	0x44
#define C_EGADD	0x48
#define C_PACK	0x4c
#define C_ALGO	0x50
#define C_OP1	0x54

; loop registers: r0-r3 E1-E4, r4 eg_timer_add, r5 ymx_sin, r6 ymx_tl,
; r7 algo routine, r8-r12 temps, r13-r16 phase1-4, r17 eg_timer, r18 mem,
; r19 op1_out, r20 buffer, r21 end, r22 ct, r23 had output (8), r24 feedback
; shift, r25 feedback mask (0 when FB = 0), fp loop head (.LloopL with LFO)

; R = operator output for index S with E (T: temp; R may be T, not S)
.macro OP S, E, T, R
	bmsk	\T,\S,8
	ldw.as	\T,[r5,\T]
	nop
	nop
	add	\T,\T,\E
	min	\T,\T,3328
	ldw.as	\R,[r6,\T]
	btst	\S,9
	nop
	rsub.ne	\R,\R,0
.endm
; two independent operators, interleaved
.macro OP2 S, E, T, R, S2, E2, T2, R2
	bmsk	\T,\S,8
	bmsk	\T2,\S2,8
	ldw.as	\T,[r5,\T]
	ldw.as	\T2,[r5,\T2]
	nop
	add	\T,\T,\E
	add	\T2,\T2,\E2
	min	\T,\T,3328
	min	\T2,\T2,3328
	ldw.as	\R,[r6,\T]
	ldw.as	\R2,[r6,\T2]
	btst	\S,9
	rsub.ne	\R,\R,0
	btst	\S2,9
	rsub.ne	\R2,\R2,0
.endm
#ifdef YMSCHED
; -DYMSCHED: OP with its load slots filled. OPT: the last operator of a sample with
; the four phase steps (X, Y: free temps); OPS: slot 1 with the op1_out shift.
.macro OPT S, E, T, R, X, Y
	bmsk	\T,\S,8
	ldw.as	\T,[r5,\T]
	ld	\X,[r22,C_INC1]
	ld	\Y,[r22,C_INC1+4]
	add	\T,\T,\E
	min	\T,\T,3328
	ldw.as	\R,[r6,\T]
	add	r13,r13,\X
	add	r14,r14,\Y
	ld	\X,[r22,C_INC1+8]
	btst	\S,9
	ld	\Y,[r22,C_INC1+12]
	rsub.ne	\R,\R,0
	add	r15,r15,\X
	add	r16,r16,\Y
.endm
.macro OPS S, E, T, R
	bmsk	\T,\S,8
	ldw.as	\T,[r5,\T]
	asl	r19,r19,16
	nop
	add	\T,\T,\E
	min	\T,\T,3328
	ldw.as	\R,[r6,\T]
	btst	\S,9
	nop
	rsub.ne	\R,\R,0
.endm
#define LASTOP(x, y)	OPT r8,r3,r10,r10,x,y
#define TAIL	.Ltail2
#else
#define LASTOP(x, y)	OP r8,r3,r10,r10
#define TAIL	.Ltail
#endif
; S = (phase >> 16) + (pm >> 1) (T: temp)
.macro IDX S, PH, PM, T
	lsr	\T,\PH,16
	asr	\S,\PM,1
	add	\S,\S,\T
.endm
.macro IDX0 S, PH
	lsr	\S,\PH,16
.endm

	.align 4
	.global ymx_loop
ymx_loop:
	st.a	blink,[sp,-4]
	st.a	fp,[sp,-4]
	st.a	r13,[sp,-4]
	st.a	r14,[sp,-4]
	st.a	r15,[sp,-4]
	st.a	r16,[sp,-4]
	st.a	r17,[sp,-4]
	st.a	r18,[sp,-4]
	st.a	r19,[sp,-4]
	st.a	r20,[sp,-4]
	st.a	r21,[sp,-4]
	st.a	r22,[sp,-4]
	st.a	r23,[sp,-4]
	st.a	r24,[sp,-4]
	st.a	r25,[sp,-4]
	mov	r22,r0
	mov	r20,r1
	add2	r21,r1,r2
	ld	r8,[r22,C_ALGO]
	ld	r9,[r22,C_PACK]
	and	r8,r8,7
	mov	r10,ymx_algos
	ld.as	r7,[r10,r8]
	lsr	r24,r9,12
	and.f	r24,r24,15
	mov	r25,0
	mov.ne	r25,-1
	st	r7,[r22,C_JMP]
	mov	r23,0
	ld	r18,[r22,C_MEM]
	ld	r19,[r22,C_OP1]
	ld	r17,[r22,C_EGT]
	ld	r13,[r22,C_PH1]
	ld	r14,[r22,C_PH1+4]
	ld	r15,[r22,C_PH1+8]
	ld	r16,[r22,C_PH1+12]
	mov	fp,.Lloop
	btst	r9,3
	mov.ne	fp,.LloopL
#ifdef YMEG3
	; -DYMEG3 (no SSG-EG): r17 = eg_timer + (ticks since the last envelope
	; update) << 16; an update (eg_cnt step) every 3 ticks = when r17 >= 3 << 16.
	; upd_cnt 0 (never updated yet) acts as 1; stored back at exit.
	btst	r9,1
	bne	1f
	ld	r8,[r22,C_ALGO]
	mov	fp,.LloopN
	btst	r9,3
	mov.ne	fp,.LloopLN
	lsr	r8,r8,4
	and	r8,r8,3
	rsub	r8,r8,3
	min	r8,r8,2
	asl	r8,r8,16
	add	r17,r17,r8
1:
#endif
	bl	.Lload
	cmp	r20,r21
	bhs	.Lexit
	j	[fp]
.LloopL:	; LFO: lfo_cnt += lfo_inc; C (ymx_lfo) when its position changes
	ld	r8,[r22,C_LFOCNT]
	ld	r9,[r22,C_LFOINC]
	nop
	nop
	add	r9,r9,r8
	st	r9,[r22,C_LFOCNT]
	xor	r8,r8,r9
	lsr	r8,r8,24
	and.f	0,r8,127
	bne	.Llfo
.Lloop:
	add	r17,r17,r4
	lsr.f	r8,r17,16
	bne	.Leg
#ifdef YMEG3
	b	.Legback
.LloopLN:	; the same LFO step, then the new envelope timer
	ld	r8,[r22,C_LFOCNT]
	ld	r9,[r22,C_LFOINC]
	nop
	nop
	add	r9,r9,r8
	st	r9,[r22,C_LFOCNT]
	xor	r8,r8,r9
	lsr	r8,r8,24
	and.f	0,r8,127
	bne	.Llfo
.LloopN:
	add	r17,r17,r4
	cmp	r17,0x30000
	bhs	.LegN
#endif
.Legback:
	; slot 1: feedback (op1_out0 + op1_out1) << FB, phase1 + it
	asl	r8,r19,16
	add	r8,r8,r19
	asr	r8,r8,16
	asl	r8,r8,r24
	and	r8,r8,r25
	add	r8,r8,r13
	lsr	r8,r8,16
#ifdef YMSCHED
	OPS	r8,r0,r9,r10
#else
	OP	r8,r0,r9,r10
	asl	r19,r19,16
#endif
	extw	r10,r10
	j.d	[r7]
	or	r19,r19,r10

; each algo routine: smp in r10, then .Ltail
	.align 4
.La0:	; M1-C1-MEM-M2-C2-OUT
	asr	r8,r19,16
	IDX	r8,r14,r8,r9
	IDX	r10,r15,r18,r9
	OP2	r8,r1,r18,r18, r10,r2,r11,r11
	IDX	r8,r16,r11,r9
	LASTOP(r9, r11)
	b	TAIL
.La1:	; mem = c1 + op2; c2 = op3(m2); smp = op4(c2)
	IDX0	r8,r14
	IDX	r10,r15,r18,r9
	OP2	r8,r1,r9,r9, r10,r2,r11,r11
	asr	r12,r19,16
	add	r18,r12,r9
	IDX	r8,r16,r11,r9
	LASTOP(r9, r11)
	b	TAIL
.La2:	; mem = op2; c2 = c1 + op3(m2); smp = op4(c2)
	IDX0	r8,r14
	IDX	r10,r15,r18,r9
	OP2	r8,r1,r18,r18, r10,r2,r11,r11
	asr	r9,r19,16
	add	r11,r11,r9
	IDX	r8,r16,r11,r9
	LASTOP(r9, r11)
	b	TAIL
.La3:	; c2 = mem + op3; mem = op2(c1); smp = op4(c2)
	mov	r12,r18
	asr	r8,r19,16
	IDX	r8,r14,r8,r9
	IDX0	r10,r15
	OP2	r8,r1,r18,r18, r10,r2,r11,r11
	add	r12,r12,r11
	IDX	r8,r16,r12,r9
	LASTOP(r9, r11)
	b	TAIL
.La4:	; smp = op2(c1) + op4(op3)
	asr	r8,r19,16
	IDX	r8,r14,r8,r9
	IDX0	r10,r15
	OP2	r8,r1,r12,r12, r10,r2,r11,r11
	IDX	r8,r16,r11,r9
	LASTOP(r9, r11)
	b.d	TAIL
	add	r10,r10,r12
.La5:	; smp = op3(m2) + op2(c1) + op4(c1); mem = c1
	asr	r12,r19,16
	IDX	r8,r14,r12,r9
	IDX	r10,r15,r18,r9
	OP2	r8,r1,r9,r9, r10,r2,r11,r11
	add	r9,r9,r11
	IDX	r8,r16,r12,r10
	mov	r18,r12
	LASTOP(r11, r12)
	b.d	TAIL
	add	r10,r10,r9
.La6:	; smp = op3 + op2(c1) + op4
	asr	r8,r19,16
	IDX	r8,r14,r8,r9
	IDX0	r10,r15
	OP2	r8,r1,r9,r9, r10,r2,r11,r11
	add	r9,r9,r11
	IDX0	r8,r16
	LASTOP(r11, r12)
	b.d	TAIL
	add	r10,r10,r9
.La7:	; smp = c1 + op3 + op2 + op4
	IDX0	r8,r14
	IDX0	r10,r15
	OP2	r8,r1,r9,r9, r10,r2,r11,r11
	add	r9,r9,r11
	IDX0	r8,r16
	LASTOP(r11, r12)
	add	r10,r10,r9
	asr	r9,r19,16
	add	r10,r10,r9
#ifdef YMSCHED
	b	.Ltail2
#endif
	; fall through
.Ltail:
	ld	r8,[r22,C_INC1]
	ld	r9,[r22,C_INC1+4]
	ld	r11,[r22,C_INC1+8]
	ld	r12,[r22,C_INC1+12]
	add	r13,r13,r8
	add	r14,r14,r9
	add	r15,r15,r11
	breq.d	r10,0,.Lnext
	add	r16,r16,r12
#ifdef YMSCHED
	b	1f
.Ltail2:
	breq	r10,0,.Lnext
1:
#endif
	; clip to 14 bits
	asr	r8,r10,13
	add	r8,r8,1
	brhi	r8,1,.Lclip
.Lclipped:
	ld	r9,[r20]
#ifdef YMEG3
	or	r23,r23,8
#else
	mov	r23,8
#endif
	add	r9,r9,r10
	st	r9,[r20]
.Lnext:
	add	r20,r20,4
	cmp	r20,r21
	jlo	[fp]
.Lexit:
#ifdef YMEG3
	ld	r8,[r22,C_PACK]
	btst	r8,1
	bne	2f
	ld	r8,[r22,C_ALGO]
	lsr	r9,r17,16
	rsub	r9,r9,3
	and.f	0,r8,0x30
	bne	1f
	btst	r23,8
	mov.eq	r9,0
1:	and	r8,r8,~0x30
	asl	r9,r9,4
	or	r8,r8,r9
	st	r8,[r22,C_ALGO]
	bmsk	r17,r17,15
	and	r23,r23,8
2:
#endif
	st	r13,[r22,C_PH1]
	st	r14,[r22,C_PH1+4]
	st	r15,[r22,C_PH1+8]
	st	r16,[r22,C_PH1+12]
	st	r17,[r22,C_EGT]
	st	r18,[r22,C_MEM]
	st	r19,[r22,C_OP1]
	ld	r8,[r22,C_ALGO]
	or	r8,r8,r23
	st	r8,[r22,C_ALGO]
	ld.ab	r25,[sp,4]
	ld.ab	r24,[sp,4]
	ld.ab	r23,[sp,4]
	ld.ab	r22,[sp,4]
	ld.ab	r21,[sp,4]
	ld.ab	r20,[sp,4]
	ld.ab	r19,[sp,4]
	ld.ab	r18,[sp,4]
	ld.ab	r17,[sp,4]
	ld.ab	r16,[sp,4]
	ld.ab	r15,[sp,4]
	ld.ab	r14,[sp,4]
	ld.ab	r13,[sp,4]
	ld.ab	fp,[sp,4]
	ld.ab	blink,[sp,4]
	j	[blink]

.Lclip:	; n < 0 ? -8192 : 8191
	asr	r8,r10,31
	mov	r10,8191
	b.d	.Lclipped
	xor	r10,r10,r8

.Llfo:
	bl.d	ymx_lfo
	mov	r0,r22
	bl	.Lload
#ifdef YMEG3
	ld	r8,[r22,C_PACK]
	btst	r8,1
	beq	.LloopN
#endif
	b	.Lloop

#ifdef YMEG3
.LegN:	; envelope update: eg_cnt step, C (ymx_eg_upd) when a slot acts
	sub	r17,r17,0x30000
	ld	r9,[r22,C_EGCNT]
	or	r23,r23,0x100
	add	r9,r9,1
	lsr.f	r10,r9,12
	mov.ne	r9,1
	st	r9,[r22,C_EGCNT]
	ld	r10,[ymx_mask]
	ld	r11,[ymx_mask+4]
	ld	r12,[ymx_mask+8]
	ld	r8,[ymx_mask+12]
	and.f	0,r9,r10
	beq	.LeguN
	and.f	0,r9,r11
	beq	.LeguN
	and.f	0,r9,r12
	beq	.LeguN
	and.f	0,r9,r8
	beq	.LeguN
.LegnN:
	cmp	r17,0x30000
	bhs	.LegN
	b	.Legback
.LeguN:
	bl.d	ymx_eg_upd
	mov	r0,r22
	bl	.Lload
	b	.LegnN
#endif

.Leg:	; envelope timer overflow: PicoDrive's EG step loop. Without SSG-EG
	; here: upd_cnt / eg_cnt, and C (ymx_eg_upd) only when a slot's
	; update_eg_phase would do something (ymx_mask: see ymloop.c)
	ld	r8,[r22,C_PACK]
	btst	r8,1
	bne	.Legc
.Legf:
	sub	r17,r17,0x10000
	ld	r8,[r22,C_ALGO]
	and.f	0,r8,0x30
	sub.ne	r8,r8,0x10
	and.f	0,r8,0x30
	bne	.Legs
	or	r8,r8,0x30
	ld	r9,[r22,C_EGCNT]
	st	r8,[r22,C_ALGO]
	add	r9,r9,1
	lsr.f	r10,r9,12
	mov.ne	r9,1
	st	r9,[r22,C_EGCNT]
	ld	r10,[ymx_mask]
	ld	r11,[ymx_mask+4]
	ld	r12,[ymx_mask+8]
	ld	r8,[ymx_mask+12]
	and.f	0,r9,r10
	beq	.Legu
	and.f	0,r9,r11
	beq	.Legu
	and.f	0,r9,r12
	beq	.Legu
	and.f	0,r9,r8
	beq	.Legu
	b	.Legn
.Legs:
	st	r8,[r22,C_ALGO]
.Legn:
	lsr.f	r8,r17,16
	bne	.Legf
	b	.Legback
.Legu:
	bl.d	ymx_eg_upd
	mov	r0,r22
	bl	.Lload
	b	.Legn
.Legc:	; with SSG-EG: the step loop in C, then reload what it may change
	st	r13,[r22,C_PH1]
	st	r14,[r22,C_PH1+4]
	st	r15,[r22,C_PH1+8]
	st	r16,[r22,C_PH1+12]
	st	r17,[r22,C_EGT]
	bl.d	ymx_eg_run
	mov	r0,r22
	ld	r13,[r22,C_PH1]
	ld	r14,[r22,C_PH1+4]
	ld	r15,[r22,C_PH1+8]
	ld	r16,[r22,C_PH1+12]
	ld	r17,[r22,C_EGT]
	bl	.Lload
	b	.Legback

; r0-r7 from ct (they don't survive a call)
.Lload:
	ldw	r0,[r22,C_E1]
	ldw	r1,[r22,C_E1+2]
	ldw	r2,[r22,C_E1+4]
	ldw	r3,[r22,C_E1+6]
	ld	r4,[r22,C_EGADD]
	mov	r5,ymx_sin
	mov	r6,ymx_tl
	j.d	[blink]
	ld	r7,[r22,C_JMP]

	.section .rodata
	.align 4
ymx_algos:
	.word	.La0, .La1, .La2, .La3, .La4, .La5, .La6, .La7
