// okdos stub: a DOS program that runs the ELF32 i386 executable appended to it in 32-bit
// protected mode through DPMI (openkal-dos, technical experiment only).
//
// THE FILE is this stub (an MZ program) followed by the executable. The stub's offsets are the
// file's: the header states the code segment and the stack segment two paragraphs below the load
// module, so CS:0 is the file's first byte and an offset here is an offset there.
//
// WHAT IT DOES, in order: frees the memory DOS gave it beyond its own; finds the program's name
// at the end of its environment and opens it; reads the executable's ELF header and program
// headers; asks for a DPMI host (int 2Fh, 1687h), and where there is none runs CWSDPMI.EXE from
// the program's directory and asks again; enters protected mode as a 32-bit client; allocates
// one block of memory for the executable's segments, its first stack and the hand-over record;
// makes a code and a data descriptor whose base is the block and whose limit spans 4 GiB; reads
// each loadable segment into the block through a transfer buffer in conventional memory (int 21h
// simulated by DPMI 0300h), zeroes what the file does not hold, and enters the executable at its
// ELF entry with EBX the hand-over record's offset (src/dos.h, okdos::handoff).
//
// Built by stub/build.sh into tools/src/stub_bytes.h, which the seal (tools/) writes before the
// executable. Apache-2.0, as the rest of openkal-dos; nothing of DJGPP's stub is used.
    .code16
    .text
    .globl _okdos_stub
_okdos_stub:
    // ── MZ header (32 bytes) ─────────────────────────────────────────────
    .ascii "MZ"
    .word (stub_end - _okdos_stub) % 512
    .word (stub_end - _okdos_stub + 511) / 512
    .word 0                         // relocations
    .word 2                         // header paragraphs
    .word (STACK + 15) / 16         // extra paragraphs needed: the stack
    .word 0xffff                    // extra paragraphs wanted
    .word 0xfffe                    // SS: two paragraphs below the load module
    .word stub_end + STACK          // SP
    .word 0                         // checksum
    .word start                     // IP
    .word 0xfffe                    // CS: two paragraphs below the load module
    .word 0x1c                      // relocation table
    .word 0                         // overlay
    .word 0, 0

    .set STACK, 0x800
    .set PHMAX, 16
    .set TB_PARAS, 0x400            // the transfer buffer: 16 KiB
    .set FIRST_STACK, 0x10000       // the executable's first stack: 64 KiB

start:
    movw %es, %ax
    movw %cs, %bx
    movw %bx, %ds
    movw %ax, psp_seg
    // Keep the stub and its stack; give the rest back so a DPMI host can be loaded.
    movw %cs, %bx
    subw %ax, %bx                   // paragraphs from the PSP to CS
    addw $((stub_end - _okdos_stub + STACK + 15) / 16), %bx
    movb $0x4a, %ah
    int $0x21
    jc fail_memory

    // The program's name: after the environment's strings, a word, then the name.
    movw psp_seg, %es
    movw %es:0x2c, %ax
    movw %ax, env_seg
    movw %ax, %es
    xorw %di, %di
1:  cmpb $0, %es:(%di)
    je 2f
3:  incw %di
    cmpb $0, %es:(%di)
    jne 3b
    incw %di
    jmp 1b
2:  addw $3, %di                    // the empty string, then the count word
    movw %di, name_off
    // Copy it, for CWSDPMI's name beside it.
    movw $path, %si
    xorw %bx, %bx                   // the last separator's position in path
4:  movb %es:(%di), %al
    movb %al, (%si)
    cmpb $'\\', %al
    jne 5f
    movw %si, %bx
5:  incw %di
    incw %si
    cmpb $0, %al
    jne 4b
    movw %bx, path_sep

    // Open the program (read only) and read the executable's headers.
    pushw %ds
    movw env_seg, %ds
    movw %cs:name_off, %dx
    movw $0x3d00, %ax
    int $0x21
    popw %ds
    jc fail_open
    movw %ax, handle
    movw $stub_end, %dx
    xorw %cx, %cx
    call seek
    movw $ehdr, %dx
    movw $52, %cx
    call read
    jc fail_read
    cmpl $0x464c457f, ehdr          // \x7fELF
    jne fail_format
    cmpw $0x0101, ehdr + 4          // 32-bit, little-endian
    jne fail_format
    cmpw $3, ehdr + 18              // EM_386
    jne fail_format
    movw ehdr + 44, %cx             // e_phnum
    cmpw $PHMAX, %cx
    ja fail_format
    movw %cx, phnum
    shlw $5, %cx                    // 32 bytes each
    movl ehdr + 28, %edx            // e_phoff
    addl $stub_end, %edx
    pushw %cx
    movl %edx, %eax
    movw %ax, %dx
    shrl $16, %eax
    movw %ax, %cx
    call seek
    popw %cx
    movw $phdrs, %dx
    call read
    jc fail_read

    // The span: the highest end of a loadable segment.
    xorl %eax, %eax
    movw phnum, %cx
    movw $phdrs, %si
6:  jcxz 7f
    cmpl $1, (%si)                  // PT_LOAD
    jne 8f
    movl 8(%si), %edx               // p_vaddr
    addl 20(%si), %edx              // p_memsz
    cmpl %eax, %edx
    jbe 8f
    movl %edx, %eax
8:  addw $32, %si
    decw %cx
    jmp 6b
7:  addl $0xfff, %eax
    andl $0xfffff000, %eax
    movl %eax, span
    addl $(FIRST_STACK + 0x1000), %eax
    movl %eax, block_size

    // A DPMI host, or CWSDPMI from the program's directory.
    call ask_dpmi
    jnc have_dpmi
    call load_cwsdpmi
    call ask_dpmi
    jc fail_dpmi
have_dpmi:
    // The host's private data, in conventional memory.
    movw host_paras, %bx
    testw %bx, %bx
    jz 9f
    movb $0x48, %ah
    int $0x21
    jc fail_memory
    movw %ax, host_seg
9:  movw host_seg, %es
    movw $1, %ax                    // a 32-bit client
    lcall *switch_entry
    jc fail_switch

    // ── Protected mode (16-bit code, selectors for the real-mode segments) ─────────────
    movw %es, psp_sel
    // The block: its linear address in BX:CX, its handle in SI:DI.
    movl block_size, %eax
    movw %ax, %cx
    shrl $16, %eax
    movw %ax, %bx
    movw $0x0501, %ax
    int $0x31
    jc fail_block
    movw %bx, base + 2
    movw %cx, base
    movw %si, block_handle + 2
    movw %di, block_handle
    // Two descriptors: code, then data.
    xorw %ax, %ax
    movw $2, %cx
    int $0x31
    jc fail_desc
    movw %ax, code_sel
    movw %ax, %bx
    movw $3, %ax
    int $0x31
    addw %bx, %ax
    movw %ax, data_sel
    // Base and limit of each; access: present, the client's privilege, 32-bit, 4 KiB granular.
    movw %cs, %ax
    andw $3, %ax
    shlw $5, %ax
    movw %ax, dpl
    movw code_sel, %bx
    call frame_descriptor
    movw dpl, %cx
    orw $0xc09a, %cx                // code: execute/read
    movw $9, %ax
    int $0x31
    jc fail_desc
    movw data_sel, %bx
    call frame_descriptor
    movw dpl, %cx
    orw $0xc092, %cx                // data: read/write
    movw $9, %ax
    int $0x31
    jc fail_desc
    // The transfer buffer.
    movw $0x0100, %ax
    movw $TB_PARAS, %bx
    int $0x31
    jc fail_memory_pm
    movw %ax, tb_seg
    movw %dx, tb_sel

    // Each loadable segment, read into the block, the rest of it zero.
    movw phnum, %cx
    movw $phdrs, %si
10: jcxz 11f
    pushw %cx
    pushw %si
    cmpl $1, (%si)
    jne 12f
    call load_segment
    jc fail_read_pm
12: popw %si
    popw %cx
    addw $32, %si
    decw %cx
    jmp 10b
11:

    // The hand-over record, at the end of the executable's span.
    movw data_sel, %es
    movl span, %edi
    movl $0x53444b4f, %es:(%edi)    // "OKDS"
    movl $1, %es:4(%edi)            // version
    movl base, %eax
    movl %eax, %es:8(%edi)
    movl block_size, %eax
    movl %eax, %es:12(%edi)
    movl block_handle, %eax
    movl %eax, %es:16(%edi)
    movw psp_sel, %ax
    movw %ax, %es:20(%edi)
    movw psp_seg, %ax
    movw %ax, %es:22(%edi)
    movw env_seg, %ax
    movw %ax, %es:24(%edi)
    movw tb_seg, %ax
    movw %ax, %es:26(%edi)
    movl $(TB_PARAS * 16), %es:28(%edi)
    movw code_sel, %ax
    movw %ax, %es:32(%edi)
    movw data_sel, %ax
    movw %ax, %es:34(%edi)
    movw tb_sel, %ax
    movw %ax, %es:36(%edi)
    movw handle, %ax                // the program's file, left open: closing it here hangs
    movw %ax, %es:38(%edi)          // under DOSBox Staging, and DOS closes it at the end
    // The name the program was started by, as DOS gave it.
    leal 40(%edi), %edi
    movw $path, %si
13: movb (%si), %al
    movb %al, %es:(%edi)
    incw %si
    incl %edi
    testb %al, %al
    jnz 13b

    // Enter: DS, ES and SS the data descriptor, the first stack at the block's end.
    movl span, %ebx
    movl ehdr + 24, %edx            // e_entry
    movw code_sel, %cx
    movw data_sel, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl %cs:block_size, %esp
    subl $16, %esp
    pushl %ecx
    pushl %edx
    lretl

// ── Real mode helpers ──────────────────────────────────────────────────────
// seek to CX:DX from the file's start
seek:
    movw handle, %bx
    movw $0x4200, %ax
    int $0x21
    ret
// read CX bytes into DS:DX; carry on error or a short read
read:
    pushw %cx
    movw handle, %bx
    movb $0x3f, %ah
    int $0x21
    popw %cx
    jc 1f
    cmpw %cx, %ax
    jne 2f
    clc
    ret
2:  stc
1:  ret
// int 2Fh 1687h: carry when there is no 32-bit host
ask_dpmi:
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz 1f
    testw $1, %bx
    jz 1f
    movw %si, host_paras
    movw %di, switch_entry
    movw %es, switch_entry + 2
    clc
    ret
1:  stc
    ret
// CWSDPMI.EXE beside the program, run as a program; it stays resident for this one.
load_cwsdpmi:
    movw path_sep, %di
    incw %di
    movw $cwsdpmi_name, %si
1:  movb (%si), %al
    movb %al, (%di)
    incw %si
    incw %di
    testb %al, %al
    jnz 1b
    movw %cs, %ax
    movw %ax, exec_tail + 2
    movw %ax, exec_fcb1 + 2
    movw %ax, exec_fcb2 + 2
    movw %ax, %es
    movw $exec_block, %bx
    movw $path, %dx
    movw %sp, saved_sp
    movw $0x4b00, %ax
    int $0x21
    movw %cs, %ax                   // DOS 2 kept nothing but CS and IP across EXEC
    movw %ax, %ds
    cli
    movw %ax, %ss
    movw saved_sp, %sp
    sti
    // The program's own name again, for what follows.
    movw env_seg, %es
    movw name_off, %si
    movw $path, %di
2:  movb %es:(%si), %al
    movb %al, (%di)
    incw %si
    incw %di
    testb %al, %al
    jnz 2b
    ret

// ── Protected mode helpers ─────────────────────────────────────────────────
// BX a descriptor: its base the block, its limit 4 GiB
frame_descriptor:
    movw base + 2, %cx
    movw base, %dx
    movw $7, %ax
    int $0x31
    movw $0xffff, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    ret
// int 21h in real mode with the registers as they are (no pointers): DPMI 0300h
dos_rm:
    pushl %edi
    movl %eax, rm_eax
    movl %ebx, rm_ebx
    movl %ecx, rm_ecx
    movl %edx, rm_edx
    movw tb_seg, %ax
    movw %ax, rm_ds
    movw $0x0202, rm_flags           // interrupts enabled: DOS may wait on them
    movw $0, rm_ss
    movw $0, rm_sp
    pushw %es
    movw %ds, %ax
    movw %ax, %es
    movl $rm_regs, %edi             // a 32-bit client's structure is at ES:EDI, all of it
    movw $0x0300, %ax
    movw $0x21, %bx
    xorw %cx, %cx
    int $0x31
    popw %es
    popl %edi
    jc 1f
    testw $1, rm_flags
    jnz 1f
    movl rm_eax, %eax
    clc
    ret
1:  stc
    ret
// one PT_LOAD at SI: its file bytes, then zero to its size in memory
load_segment:
    movl 4(%si), %eax               // p_offset
    addl $stub_end, %eax
    movl %eax, %edx
    shrl $16, %eax
    movw %ax, %cx
    movw handle, %bx
    movw $0x4200, %ax
    call dos_rm
    jc 9f
    movl 8(%si), %edi               // p_vaddr: the destination in the block
    movl 16(%si), %ebp              // p_filesz
1:  testl %ebp, %ebp
    jz 3f
    movl $(TB_PARAS * 16), %ecx
    cmpl %ecx, %ebp
    jae 2f
    movl %ebp, %ecx
2:  pushl %ecx
    movw handle, %bx
    xorl %edx, %edx
    movb $0x3f, %ah
    call dos_rm
    popl %ecx
    jc 9f
    cmpl %ecx, %eax
    jne 8f
    // copy ECX bytes from the transfer buffer to the block at EDI
    pushw %ds
    pushw %es
    pushw %si
    movw data_sel, %ax
    movw tb_sel, %dx
    movw %ax, %es
    movw %dx, %ds
    xorl %esi, %esi
    cld
    subl %ecx, %ebp
    .byte 0x67, 0xf3, 0xa4          // rep movsb, 32-bit addresses
    popw %si
    popw %es
    popw %ds
    jmp 1b
3:  movl 20(%si), %ecx              // p_memsz - p_filesz, zero
    subl 16(%si), %ecx
    jz 4f
    pushw %es
    movw data_sel, %ax
    movw %ax, %es
    xorb %al, %al
    cld
    .byte 0x67, 0xf3, 0xaa          // rep stosb, 32-bit addresses
    popw %es
4:  clc
    ret
8:  stc
9:  ret

// ── Failures: a sentence on standard error, then status 255 ────────────────
fail_memory:    movw $m_memory, %dx;  jmp fail_rm
fail_open:      movw $m_open, %dx;    jmp fail_rm
fail_read:      movw $m_read, %dx;    jmp fail_rm
fail_format:    movw $m_format, %dx;  jmp fail_rm
fail_dpmi:      movw $m_dpmi, %dx;    jmp fail_rm
fail_switch:    movw $m_switch, %dx;  jmp fail_rm
fail_rm:
    movw %cs, %ax
    movw %ax, %ds
    movw %dx, %si
    xorw %cx, %cx
1:  cmpb $0, (%si)
    je 2f
    incw %si
    incw %cx
    jmp 1b
2:  movw $2, %bx
    movb $0x40, %ah
    int $0x21
    movw $0x4cff, %ax
    int $0x21
fail_block:     movw $m_block, %dx;   jmp fail_pm
fail_desc:      movw $m_desc, %dx;    jmp fail_pm
fail_memory_pm: movw $m_memory, %dx;  jmp fail_pm
fail_read_pm:   movw $m_read, %dx;    jmp fail_pm
fail_pm:
    // In protected mode int 21h AH=40h takes DS:DX as it is only where the host translates it;
    // the message is copied to the transfer buffer and written from real mode instead, where
    // the buffer exists; without one, the status alone says it.
    movw tb_sel, %ax
    testw %ax, %ax
    jz 3f
    movw %ax, %es
    movw %dx, %si
    xorw %di, %di
    xorw %cx, %cx
1:  movb (%si), %al
    testb %al, %al
    jz 2f
    movb %al, %es:(%di)
    incw %si
    incw %di
    incw %cx
    jmp 1b
2:  movw $2, %bx
    xorw %dx, %dx
    movb $0x40, %ah
    call dos_rm
3:  movw $0x4cff, %ax
    int $0x21

m_memory:  .asciz "okdos: not enough memory\r\n"
m_open:    .asciz "okdos: the program cannot open itself\r\n"
m_read:    .asciz "okdos: the program's executable cannot be read\r\n"
m_format:  .asciz "okdos: the appended executable is not ELF32 i386 with at most 16 program headers\r\n"
m_dpmi:    .asciz "okdos: no 32-bit DPMI host, and none from CWSDPMI.EXE beside the program\r\n"
m_switch:  .asciz "okdos: the DPMI host refused protected mode\r\n"
m_block:   .asciz "okdos: the DPMI host has no block of the executable's size\r\n"
m_desc:    .asciz "okdos: the DPMI host refused the program's descriptors\r\n"
cwsdpmi_name: .asciz "CWSDPMI.EXE"

    .balign 4
psp_seg:      .word 0
psp_sel:      .word 0
env_seg:      .word 0
name_off:     .word 0
path_sep:     .word 0
handle:       .word 0
saved_sp:     .word 0
phnum:        .word 0
host_paras:   .word 0
host_seg:     .word 0
switch_entry: .word 0, 0
code_sel:     .word 0
data_sel:     .word 0
dpl:          .word 0
tb_seg:       .word 0
tb_sel:       .word 0
    .balign 4
span:         .long 0
block_size:   .long 0
base:         .long 0
block_handle: .long 0
exec_block:   .word 0
exec_tail:    .word exec_empty, 0
exec_fcb1:    .word exec_fcb, 0
exec_fcb2:    .word exec_fcb, 0
exec_empty:   .byte 0, 0x0d
exec_fcb:     .space 16, 0
rm_regs:
rm_edi:   .long 0
rm_esi:   .long 0
rm_ebp:   .long 0
          .long 0
rm_ebx:   .long 0
rm_edx:   .long 0
rm_ecx:   .long 0
rm_eax:   .long 0
rm_flags: .word 0
rm_es:    .word 0
rm_ds:    .word 0
rm_fs:    .word 0
rm_gs:    .word 0
rm_ip:    .word 0
rm_cs:    .word 0
rm_sp:    .word 0
rm_ss:    .word 0
ehdr:     .space 52, 0
path:     .space 128, 0
    .balign 4
phdrs:    .space 32 * PHMAX, 0
    .balign 16
stub_end:
