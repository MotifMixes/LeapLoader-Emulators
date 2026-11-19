; arcdraw.S: the hand-written ARC600 parts of the -DARCDRAW renderer (drawx.c).
; The line buffer has one byte per LCD column (column 2i of 320), PicoDrive's
; HighCol encoding (bits 0-5 colour, 0x40 highlight/operator, 0x80 shadow).
; A tile row is 8 pixels of 4 bits in a 32-bit word: pixel k at bit 12-4k
; (k < 4) or 44-4k. 4 of them are shown: k = 0,2,4,6 when the tile starts
; on an even column; the plane routines rotate odd/flipped rows so that the
; 4 shown pixels always sit at bits 12,4,28,20 (normal) or 16,24,0,8 (h-flip).
; No zero-overhead loops (lp), no gp.

	.text

#ifdef OUTQ
; One 8-pixel group of the pending line (-DOUTQ, drawx.c arcdraw_q) to the
; LCD: S line bytes (+= 8), D LCD words (+= 12, st.di: posted), P arcdraw_pal,
; (the LCD's 12 bpp: each 2 columns = 3 bytes g0b0, r0r1, g1b1; even columns through arcdraw_pal
; (gb | r << 12), odd ones through arcdraw_pal + 1024 (r | gb << 8): 8 pixels = 3 words
; e0 | o1 << 8 | e2 << 24, e2 >> 8 | o3 | e4 << 16 | o5 << 24, o5 >> 8 | e6 << 8 | o7 << 16)
; N groups left (-= 1; the caller checks N != 0); T, U, V scratch.
; Same words as arcdraw_out.
.macro QGRP S, D, P, N, A, B, C, E, F, G, H
	bset	\H,\P,10		; the odd columns' palette: arcdraw_pal + 1024 (2 KB aligned)
	ldb	\A,[\S,0]
	ldb	\B,[\S,1]
	ldb	\C,[\S,2]
	ld.as	\A,[\P,\A]
	ld.as	\B,[\H,\B]
	ld.as	\C,[\P,\C]
	ldb	\E,[\S,3]
	asl	\B,\B,8
	or	\A,\A,\B
	asl	\B,\C,24
	or	\A,\A,\B
	st.di	\A,[\D,0]
	ldb	\F,[\S,4]
	ldb	\G,[\S,5]
	ld.as	\E,[\H,\E]
	ld.as	\F,[\P,\F]
	ld.as	\G,[\H,\G]
	ldb	\A,[\S,6]
	ldb	\B,[\S,7]
	ld.as	\A,[\P,\A]
	ld.as	\B,[\H,\B]
	lsr	\C,\C,8
	or	\C,\C,\E
	asl	\F,\F,16
	or	\C,\C,\F
	asl	\E,\G,24
	or	\C,\C,\E
	add	\S,\S,8
	st.di	\C,[\D,4]
	lsr	\G,\G,8
	asl	\A,\A,8
	or	\G,\G,\A
	asl	\B,\B,16
	or	\G,\G,\B
	sub	\N,\N,1
	st.di	\G,[\D,8]
	add	\D,\D,12
.endm
#endif

; int-sized fields of struct pl_args (drawx.c)
#define A_LB	0	/* u8 *: line position of the first tile's first shown pixel */
#define A_NT	4	/* const u16 *: nametable row */
#define A_TX	8	/* first tile column */
#define A_XM	12	/* tile column mask */
#define A_VRAM	16	/* VRAM base */
#define A_TY	20	/* byte offset of the pixel row in a tile (4 * row) */
#define A_CNT	24	/* tiles */
#define A_PAL	28	/* or'd into low tile pixels (0x80: shadow in s/h mode) */
#define A_ROTN	32	/* rotation for normal rows (28 on odd columns) */
#define A_ROTF	36	/* rotation for h-flipped rows (4 on odd columns) */
#define A_HC	40	/* u32 *: high tile cache {line pos, code, rotated row} */
#define A_BLANK	44	/* cache blank high tiles too (s/h mode) */
#define A_ALLHI	48	/* out: 1 if no low tile was seen */
#define A_HCEND	52	/* out: end of the cache */
#define A_MASK	56	/* out (-DLREUSE): bit (tile >> 6) & 31 of every tile read */

; void arcdraw_plane(struct pl_args *a): low priority tiles of a plane (or
; window) line into the line buffer, high priority ones into the cache.
	.align 4
	.global arcdraw_plane
arcdraw_plane:
	st.a	r13,[sp,-4]
	st.a	r14,[sp,-4]
	st.a	r15,[sp,-4]
	st.a	r16,[sp,-4]
	st.a	r17,[sp,-4]
	st.a	r18,[sp,-4]
	ld	r1,[r0,A_LB]
	ld	r2,[r0,A_NT]
	ld	r3,[r0,A_TX]
	ld	r4,[r0,A_XM]
	ld	r5,[r0,A_VRAM]
	ld	r6,[r0,A_TY]
	xor	r7,r6,28		; row offset when v-flipped
	ld	r8,[r0,A_CNT]
	ld	r9,[r0,A_PAL]
	ld	r10,[r0,A_ROTN]
	ld	r11,[r0,A_ROTF]
	ld	r12,[r0,A_HC]
#ifdef LREUSE
	mov	r13,0			; tile group mask (A_BLANK == (A_PAL != 0))
#else
	ld	r13,[r0,A_BLANK]
#endif
	mov	r14,1
#ifdef OUTQ
	st.a	r19,[sp,-4]
	st.a	r20,[sp,-4]
	st.a	r21,[sp,-4]
	st.a	r22,[sp,-4]
	st.a	r23,[sp,-4]
	st.a	r24,[sp,-4]
	st.a	r25,[sp,-4]
	ld	r19,[arcdraw_q]
	ld	r20,[arcdraw_q+4]
	ld	r22,[arcdraw_q+8]
	mov	r21,arcdraw_pal
#endif
	cmp	r8,1
	blt	9f
1:
	and	r18,r3,r4
	ldw.as	r15,[r2,r18]		; tile code
	add	r3,r3,1
#if defined(PLPIPE) && defined(OUTQ)
	tst	r22,r22		; (-DPLPIPE) fills the load-use slot; no flag write until the bne below
#endif
	and	r18,r15,0x7ff
	asl	r18,r18,5
	bbit1	r15,12,5f
	add	r18,r18,r6
2:	ld	r16,[r5,r18]		; 8 pixels
#ifdef LREUSE
	lsr	r17,r15,6
	bset	r13,r13,r17
#endif
	bbit1	r15,15,6f		; high priority: cache it
	mov	r14,0
	breq	r16,0,8f
	lsr	r17,r15,9
	and	r17,r17,0x30
	bbit1	r15,11,7f
	or	r17,r17,r9		; palette | shadow
	ror	r16,r16,r10
	lsr	r18,r16,12
	and	r18,r18,15
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,0]
3:	lsr	r18,r16,4
	and	r18,r18,15
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,1]
3:	lsr	r18,r16,28
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,2]
3:	lsr	r18,r16,20
	and	r18,r18,15
	breq	r18,0,8f
	or	r18,r18,r17
	stb	r18,[r1,3]
8:
#ifdef OUTQ
#ifndef PLPIPE
	tst	r22,r22
#endif
	; after each tile, while the pending line lasts:
	bne	72f			; its next 8 pixels
80:
#endif
	sub.f	r8,r8,1
	bne.d	1b
	add	r1,r1,4
9:	st	r14,[r0,A_ALLHI]
#ifdef LREUSE
	st	r13,[r0,A_MASK]
#endif
	st	r12,[r0,A_HCEND]
#ifdef OUTQ
	st	r19,[arcdraw_q]
	st	r20,[arcdraw_q+4]
	st	r22,[arcdraw_q+8]
	ld.ab	r25,[sp,4]
	ld.ab	r24,[sp,4]
	ld.ab	r23,[sp,4]
	ld.ab	r22,[sp,4]
	ld.ab	r21,[sp,4]
	ld.ab	r20,[sp,4]
	ld.ab	r19,[sp,4]
#endif
	ld.ab	r18,[sp,4]
	ld.ab	r17,[sp,4]
	ld.ab	r16,[sp,4]
	ld.ab	r15,[sp,4]
	ld.ab	r14,[sp,4]
	ld.ab	r13,[sp,4]
	j	[blink]

5:	b.d	2b			; v-flip
	add	r18,r18,r7

#ifdef LREUSE
6:	brne	r9,0,61f		; high tile
#else
6:	brne	r13,0,61f		; high tile
#endif
	breq	r16,0,8b
61:	bbit1	r15,11,62f
	b.d	63f
	ror	r16,r16,r10
62:	ror	r16,r16,r11
63:	st	r1,[r12,0]
	st	r15,[r12,4]
	st	r16,[r12,8]
	b.d	8b
	add	r12,r12,12

7:	or	r17,r17,r9		; h-flip
	ror	r16,r16,r11
	lsr	r18,r16,16
	and	r18,r18,15
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,0]
3:	lsr	r18,r16,24
	and	r18,r18,15
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,1]
3:	and	r18,r16,15
	breq	r18,0,3f
	or	r18,r18,r17
	stb	r18,[r1,2]
3:	lsr	r18,r16,8
	and	r18,r18,15
	breq	r18,0,8b
	or	r18,r18,r17
	b.d	8b
	stb	r18,[r1,3]

#ifdef OUTQ
72:	QGRP	r19,r20,r21,r22,r15,r16,r17,r18,r23,r24,r25
	b	80b
#endif

; void arcdraw_hi(const u32 *hc, int n, int shclr, u8 *lb): the cached high
; tiles. shclr (s/h mode): clear the shadow bit under each tile first, except
; (as PicoDrive) under a first partial tile that is also the last one cached.
	.align 4
	.global arcdraw_hi
arcdraw_hi:
	brlt	r1,1,9f
1:	ld	r12,[r0,0]		; line position
	ld	r4,[r0,4]		; code
	ld	r5,[r0,8]		; rotated row
	add	r0,r0,12
	sub	r1,r1,1
	breq	r2,0,4f
	brne	r1,0,2f
	brlo	r12,r3,4f
2:	ldb	r7,[r12,0]
	bclr	r7,r7,7
	stb	r7,[r12,0]
	ldb	r7,[r12,1]
	bclr	r7,r7,7
	stb	r7,[r12,1]
	ldb	r7,[r12,2]
	bclr	r7,r7,7
	stb	r7,[r12,2]
	ldb	r7,[r12,3]
	bclr	r7,r7,7
	stb	r7,[r12,3]
4:	breq	r5,0,8f
	lsr	r6,r4,9
	bbit1	r4,11,7f
	and	r6,r6,0x30
	lsr	r7,r5,12
	and	r7,r7,15
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,0]
3:	lsr	r7,r5,4
	and	r7,r7,15
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,1]
3:	lsr	r7,r5,28
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,2]
3:	lsr	r7,r5,20
	and	r7,r7,15
	breq	r7,0,8f
	or	r7,r7,r6
	stb	r7,[r12,3]
8:	brne	r1,0,1b
9:	j	[blink]

7:	and	r6,r6,0x30		; h-flip
	lsr	r7,r5,16
	and	r7,r7,15
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,0]
3:	lsr	r7,r5,24
	and	r7,r7,15
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,1]
3:	and	r7,r5,15
	breq	r7,0,3f
	or	r7,r7,r6
	stb	r7,[r12,2]
3:	lsr	r7,r5,8
	and	r7,r7,15
	breq	r7,0,8b
	or	r7,r7,r6
	b.d	8b
	stb	r7,[r12,3]

; void arcdraw_out(u8 *dst, const u8 *line, const u32 *pal): 160 line bytes
; through the palettes to the LCD (QGRP's words), stored with st.di (uncached).
	.align 4
	.global arcdraw_out
arcdraw_out:
	mov	r3,20
1:	QGRP	r1,r0,r2,r3,r4,r5,r6,r7,r8,r9,r10
	brne	r3,0,1b
	j	[blink]

#ifdef OUTQ
; void arcdraw_qfin(void) / arcdraw_flush(void): the rest of the pending line.
	.align 4
	.global arcdraw_qfin
	.global arcdraw_flush
arcdraw_qfin:
arcdraw_flush:
	ld	r3,[arcdraw_q+8]
	breq	r3,0,2f
	ld	r0,[arcdraw_q]
	ld	r1,[arcdraw_q+4]
	mov	r2,arcdraw_pal
1:	QGRP	r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10
	brne	r3,0,1b
	st	r3,[arcdraw_q+8]
2:	j	[blink]
#endif

; void arcdraw_spr(const u8 *sprited, const s32 *pre, int prio, int sh,
;                  u8 *lb, const u8 *vram, int line): PicoDrive's
; DrawAllSprites for one line (the caller checks the tile-overflow masking):
; the line's sprites of priority prio, last to first; the last one limited to
; sprited[4+cnt] tiles. sh: s/h low pass (palette | 0x80; colour 14 clears it;
; palette 3 colours 14/15 only mark 0x40). Sprite tiles at x 1..327 (+8) are
; drawn whole: the line buffer has 8 spare bytes each side.
.macro EXT d, s, n
 .if \n == 0
	and	\d,\s,15
 .elseif \n == 28
	lsr	\d,\s,28
 .else
	lsr	\d,\s,\n
	and	\d,\d,15
 .endif
.endm
.macro PNORM o, n
	EXT	r10,r24,\n
	breq	r10,0,1f
	or	r10,r10,r20
	stb	r10,[r23,\o]
1:
.endm
.macro PNONSH o, n
	EXT	r10,r24,\n
	breq	r10,0,1f
	or	r11,r10,r20
	brne	r10,14,2f
	bclr	r11,r11,7
2:	stb	r11,[r23,\o]
1:
.endm
.macro PMARK o, n
	EXT	r10,r24,\n
	breq	r10,0,1f
	brhs	r10,14,2f
	or	r10,r10,r20
	b.d	1f
	stb	r10,[r23,\o]
2:	ldb	r10,[r23,\o]
	bset	r10,r10,6
	stb	r10,[r23,\o]
1:
.endm

	.align 4
	.global arcdraw_spr
arcdraw_spr:
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
	ldb	r7,[r0,0]
	and	r7,r7,0x7f		; sprites on the line
	add	r9,r0,4			; entries
	ldb	r8,[r9,r7]		; tile limit of the last one
s_next:	sub	r7,r7,1
	tst	r7,r7
	blt	s_done
	ldb	r10,[r9,r7]
	lsr	r11,r10,7
	cmp	r11,r2
	bne	s_skip
	and	r10,r10,0x7f
	asl	r10,r10,3
	add	r10,r10,r1
	ld	r12,[r10,0]		; hhhhvvvv -lllllll yyyyyyyy yyyyyyyy
	ld	r13,[r10,4]		; x+8 (16) | pccvhnnn nnnnnnnn
	asr	r14,r13,16		; sx
	lsr	r15,r12,28		; width
	lsr	r16,r12,24
	and	r16,r16,7		; height
	sexw	r12,r12			; y
	sub	r17,r6,r12		; row
	bbit0	r13,12,1f
	asl	r18,r16,3		; v-flip
	sub	r18,r18,1
	sub	r17,r18,r17
1:	asr	r18,r17,3
	add	r18,r18,r13		; tile
	mov	r19,r16			; delta
	bbit0	r13,11,2f
	sub	r20,r15,1		; h-flip: tile += height * (width - 1)
	bbit0	r20,0,21f
	add	r18,r18,r16
21:	bbit0	r20,1,22f
	add1	r18,r18,r16
22:	rsub	r19,r19,0
2:	and	r18,r18,0x7ff
	asl	r18,r18,5
	and	r20,r17,7
	add2	r18,r18,r20		; byte address of the row in the first tile
	asl	r19,r19,5
	breq	r8,0,3f
	mov	r15,r8			; tile limit
3:	mov	r8,0
	lsr	r20,r13,9
	and	r20,r20,0x30		; palette
	mov	r21,0			; mode: 0 normal, 1 s/h low, 2 s/h low operator
	breq	r3,0,4f
	bset	r20,r20,7
	mov	r21,1
	and	r22,r13,0x6000
	cmp	r22,0x6000
	bne	4f
	mov	r21,2
4:	and	r12,r14,1		; odd column: rotate (28 normal, 4 flipped)
	bbit1	r13,11,5f
	breq	r12,0,6f
	b.d	6f
	mov	r12,28
5:	asl	r12,r12,2
6:	sub	r23,r14,7
	asr	r23,r23,1
	add	r23,r23,r4		; line position
s_tile:	cmp	r15,1
	blt	s_next
	brlt	r14,1,s_tnext		; x <= 0: skip
	cmp	r14,328
	bge	s_next			; off the right
	bmsk	r24,r18,15
	ld	r24,[r5,r24]
	breq	r24,0,s_tnext
	ror	r24,r24,r12
	bbit1	r13,11,s_flip
	tst	r21,r21
	bne	s_nsh
	PNORM	0,12
	PNORM	1,4
	PNORM	2,28
	PNORM	3,20
s_tnext:
	add	r14,r14,8
	add	r18,r18,r19
	add	r23,r23,4
	b.d	s_tile
	sub	r15,r15,1
s_skip:	b.d	s_next
	mov	r8,0
s_done:	ld.ab	r24,[sp,4]
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
	j	[blink]

s_flip:	tst	r21,r21
	bne	s_fnsh
	PNORM	0,16
	PNORM	1,24
	PNORM	2,0
	PNORM	3,8
	b	s_tnext
s_nsh:	cmp	r21,1
	bne	s_mark
	PNONSH	0,12
	PNONSH	1,4
	PNONSH	2,28
	PNONSH	3,20
	b	s_tnext
s_mark:	PMARK	0,12
	PMARK	1,4
	PMARK	2,28
	PMARK	3,20
	b	s_tnext
s_fnsh:	cmp	r21,1
	bne	s_fmark
	PNONSH	0,16
	PNONSH	1,24
	PNONSH	2,0
	PNONSH	3,8
	b	s_tnext
s_fmark:
	PMARK	0,16
	PMARK	1,24
	PMARK	2,0
	PMARK	3,8
	b	s_tnext

#ifdef PLBW
; void arcdraw_planeb(struct pl_args *a) (-DPLBW): plane B, the first layer
; drawn, as whole words: every shown pixel is stored, a transparent one as
; (palette << 4 | pal_or) whose low nibble 0 the output palette maps to the
; background colour (drawx.c plbw_apply), so the line needs no fill first.
; High tiles store bg bytes and are cached as in arcdraw_plane. Tile words
; go to aligned words through a funnel (r7 carry, r27 = 8 * (lbpos & 3),
; blink = 32 - r27); A_PW (+60): the 4 palette words (pal << 4 | pal_or) * 0x01010101.
#define A_PW	60
.macro PBODY F
1:	and	r18,r3,r4
	ldw.as	r15,[r2,r18]		; tile code
	add	r3,r3,1
#if defined(PLPIPE) && defined(OUTQ)
	tst	r22,r22
#endif
	and	r18,r15,0x7ff
	asl	r18,r18,5
	add	r18,r18,r6
	bbit1	r15,12,5f
2:	ld	r16,[r5,r18]		; 8 pixels
#ifdef LREUSE
	lsr	r17,r15,6
	bset	r13,r13,r17
#endif
	bbit1	r15,15,6f		; high priority: cache it
	lsr	r17,r15,13
	and	r17,r17,3
	ld.as	r17,[r0,r17]		; palette word
	mov	r14,0
	bbit1	r15,11,7f
	ror	r16,r16,r10		; shown pixels at bits 12,4,28,20
	lsr	r18,r16,12
	and	r18,r18,0x000f000f
	asl	r16,r16,4
	and	r16,r16,0x0f000f00
	or	r16,r16,r18
	or	r16,r16,r17
8:
.if \F
	asl	r18,r16,r27
	or	r18,r18,r7
	st	r18,[r1,0]
	lsr	r7,r16,blink
.else
	st	r16,[r1,0]
.endif
#ifdef OUTQ
#ifndef PLPIPE
	tst	r22,r22
#endif
	bne	72f
80:
#endif
	sub.f	r8,r8,1
	bne.d	1b
	add	r1,r1,4
.if \F
	b.d	9f
	st	r7,[r1,0]
.else
	b	9f
.endif
5:	b.d	2b			; v-flip
	xor	r18,r18,28
6:	brne	r9,0,61f		; high tile (s/h: blank ones too)
	breq	r16,0,64f
61:	bbit1	r15,11,62f
	b.d	63f
	ror	r16,r16,r10
62:	ror	r16,r16,r11
	ror	r16,r16,16
63:	lsr	r17,r27,3
	add	r17,r17,r1
	st	r17,[r12,0]
	st	r15,[r12,4]
	st	r16,[r12,8]
	add	r12,r12,12
64:	b.d	8b
	ld	r16,[r0,0]		; background bytes
7:	ror	r16,r16,r11		; h-flip: pixels at bits 0,8,16,24
	and	r16,r16,0x0f0f0f0f
	b.d	8b
	or	r16,r16,r17
#ifdef OUTQ
72:	QGRP	r19,r20,r21,r22,r15,r16,r17,r18,r23,r24,r25
	b	80b
#endif
.endm

	.align 4
	.global arcdraw_planeb
arcdraw_planeb:
	st.a	r13,[sp,-4]
	st.a	r14,[sp,-4]
	st.a	r15,[sp,-4]
	st.a	r16,[sp,-4]
	st.a	r17,[sp,-4]
	st.a	r18,[sp,-4]
	nop				; (was the save of r26: gp is the BIOS's)
	st.a	r27,[sp,-4]
	st.a	blink,[sp,-4]
	ld	r1,[r0,A_LB]
	ld	r2,[r0,A_NT]
	ld	r3,[r0,A_TX]
	ld	r4,[r0,A_XM]
	ld	r5,[r0,A_VRAM]
	ld	r6,[r0,A_TY]
	nop
	ld	r8,[r0,A_CNT]
	ld	r9,[r0,A_PAL]
	ld	r10,[r0,A_ROTN]
	ld	r11,[r0,A_ROTF]
	ld	r12,[r0,A_HC]
	add	r11,r11,16
	and	r27,r1,3
	asl	r27,r27,3
	rsub	blink,r27,32
	bic	r1,r1,3
	mov	r7,0
	mov	r13,0
	mov	r14,1
	add	r0,r0,A_PW
#ifdef OUTQ
	st.a	r19,[sp,-4]
	st.a	r20,[sp,-4]
	st.a	r21,[sp,-4]
	st.a	r22,[sp,-4]
	st.a	r23,[sp,-4]
	st.a	r24,[sp,-4]
	st.a	r25,[sp,-4]
	ld	r19,[arcdraw_q]
	ld	r20,[arcdraw_q+4]
	ld	r22,[arcdraw_q+8]
	mov	r21,arcdraw_pal
#endif
	cmp	r8,1
	blt	9f
	cmp	r27,0
	beq	20f
	PBODY	1
20:	PBODY	0
9:	sub	r0,r0,A_PW
	st	r14,[r0,A_ALLHI]
#ifdef LREUSE
	st	r13,[r0,A_MASK]
#endif
	st	r12,[r0,A_HCEND]
#ifdef OUTQ
	st	r19,[arcdraw_q]
	st	r20,[arcdraw_q+4]
	st	r22,[arcdraw_q+8]
	ld.ab	r25,[sp,4]
	ld.ab	r24,[sp,4]
	ld.ab	r23,[sp,4]
	ld.ab	r22,[sp,4]
	ld.ab	r21,[sp,4]
	ld.ab	r20,[sp,4]
	ld.ab	r19,[sp,4]
#endif
	ld.ab	blink,[sp,4]
	ld.ab	r27,[sp,4]
	nop
	ld.ab	r18,[sp,4]
	ld.ab	r17,[sp,4]
	ld.ab	r16,[sp,4]
	ld.ab	r15,[sp,4]
	ld.ab	r14,[sp,4]
	ld.ab	r13,[sp,4]
	j	[blink]
#endif
