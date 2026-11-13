// The executable's entry: the stub (stub/stub.S) jumps here in 32-bit protected mode with EBX the
// offset of its hand-over record, and a first stack at the end of its block.
    .section .text.okdos_entry, "ax"
    .globl okdos_entry
    .type okdos_entry, @function
okdos_entry:
    xorl %ebp, %ebp
    andl $-16, %esp
    subl $12, %esp
    pushl %ebx
    call okdos_start
    ud2
    .size okdos_entry, . - okdos_entry
    .section .note.GNU-stack, "", @progbits
