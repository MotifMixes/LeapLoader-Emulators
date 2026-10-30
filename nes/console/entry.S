; Start of the NES app's native code (linked at the image's code address).
; The image's 0x1009 resource points at `blob`: the Flash player finds the
; native __llapp() there, and the main movie's first frame calls it.
    .cpu    arc600
    .section .text.start, "ax"
    .global _start
_start:
; ---- 0x1009 resource: {0x100, 0, count << 16, pairs}, then the (name, function) pair
blob:
    .word   0x100, 0, 0x00010000, pairs
pairs:
    .word   name, llapp_native
name:
    .asciz  "__llapp"
    .align  4

; ---- native __llapp(ctx): never returns. gp stays the BIOS's (every BIOS call
; needs it, and so may interrupts); the C code is built without small data.
; Runs on the Flash player task's own stack (the RTOS checks a task's stack
; pointer against its stack on task switches).
llapp_native:
    st      sp, [ll_bios_sp]
    bl      llmain
1:  b       1b

    .data
    .align  4
    .global ll_bios_sp
ll_bios_sp:
    .word   0
