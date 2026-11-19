; sn.S (-DSNASM, with -DSNX -DSNQ): the mono sample loop of snx.c's SN76496Update
; in ARC asm; same arithmetic as SNX_TONE / the noise loop / the mix there.
; void snx_loop(int *st, short *buffer, int length)
; st: c0-c3, o0-o3, per0-per3, vol0-vol3, rng, fb (ints; updated)
; Tone channels with volume 0 are skipped (-DSNQ: C = O = 0 set by the caller).
; No lp, no gp.
	.text
	.global	snx_loop

; registers: r0 st, r1 buffer, r2 length; c0-c3 r3-r6, o0-o3 r7-r10, per0-3 r11-r14,
; vol0-3 r15-r18, rng r19, fb r20, out r21, V r22, t r23, left r24, r25 temp
#define STEP 0x10000

; one tone channel (SNX_TONE), out += V * vol
.macro TONE C, O, P, VOL
	breq	\VOL,0,9f
	mov	r22,0
	tst	\O,\O
	mov.ne	r22,\C
	sub	\C,\C,STEP
	asl	r23,\P,1
	rsub	r23,r23,0
	brge	\C,r23,3f
	add	r22,r22,STEP/2		; C < -2 * per: settle
	mov	\C,0
	b.d	5f
	mov	\O,0
3:	brge	\C,0,4f			; while (C < 0)
	add	\C,\C,\P
	brlt	\C,0,6f
	xor	\O,\O,1
	tst	\O,\O
	b.d	4f
	add.ne	r22,r22,\P
6:	add	\C,\C,\P
	b.d	3b
	add	r22,r22,\P
4:	tst	\O,\O
	sub.ne	r22,r22,\C
5:	mul64	0,r22,\VOL
	add	r21,r21,r58
9:
.endm

	.align 4
snx_loop:
	brgt	r2,0,1f
	j	[blink]
1:
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
	ld	r3,[r0,0]
	ld	r4,[r0,4]
	ld	r5,[r0,8]
	ld	r6,[r0,12]
	ld	r7,[r0,16]
	ld	r8,[r0,20]
	ld	r9,[r0,24]
	ld	r10,[r0,28]
	ld	r11,[r0,32]
	ld	r12,[r0,36]
	ld	r13,[r0,40]
	ld	r14,[r0,44]
	ld	r15,[r0,48]
	ld	r16,[r0,52]
	ld	r17,[r0,56]
	ld	r18,[r0,60]
	ld	r19,[r0,64]
	ld	r20,[r0,68]
.Lsample:
	mov	r21,0
	TONE	r3,r7,r11,r15
	TONE	r4,r8,r12,r16
	TONE	r5,r9,r13,r17
	; noise
	mov	r24,STEP
	mov	r22,0
	tst	r10,r10
	mov.ne	r22,r6
1:	min	r23,r6,r24		; nextevent
	sub	r6,r6,r23
	brgt	r6,0,2f
	and.f	r10,r19,1
	lsr	r19,r19,1
	xor.ne	r19,r19,r20
	add.ne	r22,r22,r14
	add	r6,r6,r14
2:	sub	r24,r24,r23
	brle	r24,0,3f
	brne	r18,0,1b
3:	tst	r10,r10
	sub.ne	r22,r22,r6
	breq	r18,0,4f
	mul64	0,r22,r18
	add	r21,r21,r58
4:	; clip (unsigned), / STEP, add to the buffer
	mov	r25,0x48000000
	cmp	r21,r25
	mov.hi	r21,r25
	ldw	r23,[r1]
	lsr	r21,r21,16
	add	r23,r23,r21
	stw	r23,[r1]
	sub.f	r2,r2,1
	bne.d	.Lsample
	add	r1,r1,2
	st	r3,[r0,0]
	st	r4,[r0,4]
	st	r5,[r0,8]
	st	r6,[r0,12]
	st	r7,[r0,16]
	st	r8,[r0,20]
	st	r9,[r0,24]
	st	r10,[r0,28]
	st	r19,[r0,64]
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
.Lret:
	j	[blink]
