; k68ij.S (-DK68IDLEJ): k68's handler for the opcode group 0x46f8-0x46ff (move to SR).
; At move.w #imm,sr (0x46fc) k68c.c's k68_ijtry may finish the slice inside Gunstar Heroes'
; idle loop; otherwise (and for the other opcodes) k68i.S's own handler runs. Cold section.
; No lp, no gp.
#ifdef K68IDLEJ
	.section .xtext.k68ij,"ax",@progbits
	.global k68_ij_stub
	.align 4
k68_ij_stub:
	cmp	r17,0x46fc
	bne	9f
	sub	r0,r14,2		; ctx->PC at the opcode
	st	r0,[r13,0x60]
	st	r15,[r13,0x64]
	st	r16,[r13,0x54]
	bl	k68_flags_out
	bl.d	k68_ijtry
	sub	r0,r13,0x20
	mov	r23,r0
	ld	r14,[r13,0x60]
	ld	r15,[r13,0x64]
	ld	r16,[r13,0x54]
	bl	k68_flags_in
	breq	r23,0,1f
	b	k68_done		; counter <= 0: the slice ends in the loop
1:	add	r14,r14,2
9:	ld	r1,[k68_ij_orig]
	j	[r1]
#endif
