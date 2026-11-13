// A fault of the program, reported (openkal-dos 0.1.0). The DPMI host calls a processor-exception
// handler (int 31h, 0203h) on its own stack, with this frame (DPMI 0.9, a 32-bit client):
//
//   [esp+00h] the host's return EIP   [esp+04h] its CS    [esp+08h] the error code
//   [esp+0Ch] the faulting EIP        [esp+10h] its CS    [esp+14h] EFLAGS
//   [esp+18h] the faulting ESP        [esp+1Ch] its SS
//
// The handler records the vector, the error code and the faulting EIP, and returns to the host with
// the frame changed so that the program resumes at okdos_fault_entry on a stack of its own: the
// report runs as ordinary code, whatever the fault did to the stack it had. DS at the fault is not
// trusted; the data selector is read through CS, whose descriptor has the same base and is
// readable.
    .set OKDOS_FAULT_STACK, 16384
    .text

    .macro okdos_vector n
    .globl okdos_fault_\n
    .type okdos_fault_\n, @function
okdos_fault_\n:
    pushl $\n
    jmp okdos_fault_common
    .endm

    okdos_vector 0
    okdos_vector 6
    okdos_vector 12
    okdos_vector 13
    okdos_vector 14

okdos_fault_common:
    pushl %eax
    pushl %ds
    movw %cs:okdos_fault_ds, %ax
    movw %ax, %ds
    // [esp] DS, +04h EAX, +08h the vector, +0Ch the host's EIP, +10h its CS, +14h the error code,
    // +18h the faulting EIP, +1Ch its CS, +20h EFLAGS, +24h ESP, +28h SS
    movl 0x08(%esp), %eax
    movl %eax, okdos_fault_vector
    movl 0x14(%esp), %eax
    movl %eax, okdos_fault_error
    movl 0x18(%esp), %eax
    movl %eax, okdos_fault_eip
    movl $okdos_fault_entry, 0x18(%esp)
    movl $okdos_fault_stack + OKDOS_FAULT_STACK, 0x24(%esp)
    movzwl okdos_fault_ds, %eax
    movl %eax, 0x28(%esp)
    popl %ds
    popl %eax
    addl $4, %esp
    lret

okdos_fault_entry:
    movw %cs:okdos_fault_ds, %ax
    movw %ax, %ds
    movw %ax, %es
    andl $-16, %esp
    call okdos_fault
    ud2

    .bss
    .p2align 4
okdos_fault_stack:
    .space OKDOS_FAULT_STACK

    .section .note.GNU-stack, "", @progbits
