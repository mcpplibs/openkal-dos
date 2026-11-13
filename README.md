# openkal-dos

An implementation of [openkal][kal] on DOS, through a DPMI host.

**TECHNICAL EXPERIMENT ONLY.** This package exists to test that openkal's
interfaces, its C library and its C++ run-time reach a system as distant from
the others as DOS. It carries no stability guarantee and no support; its
interfaces, its hand-over and its stub may change in any release.

A program names the target and nothing else. The row `i686-dos-musl`, its
compile and link conventions, its seal and its runner are the openkal
platform's ([openkal-platform][plat] 0.28.0): the same C library and C++
run-time as on every other system, openkal-musl and openkal-llvm-runtime, with
this implementation beneath them, so a DOS program is written in C++23 with
`import std`:

```toml
[target.i686-dos-musl]
platform = "openkal-platform@0.28.0"
```

```sh
mcpp build --target i686-dos-musl      # hello.exe: the stub and an ELF32 i386 executable
mcpp run   --target i686-dos-musl      # run by dirun under DOSBox Staging, with CWSDPMI beside it
```

`examples/hello` sorts its arguments, throws and catches an exception across a
frame, reads `PATH` and returns 7.

## The program

A DOS program of this arrangement is two parts in one file:

| part | what it is |
|---|---|
| the stub (`stub/stub.S`, 2544 bytes) | an MZ program in 16-bit code |
| the executable | ELF32, i386, linked at its address (`ET_EXEC`, base 0x1000), at most 16 program headers |

The stub finds a 32-bit DPMI host (int 2Fh, 1687h), and where there is none it
runs `CWSDPMI.EXE` from the program's own directory and asks again. It enters
protected mode as a 32-bit client, allocates one block for the executable's
loadable segments, its first stack (64 KiB) and the hand-over record, makes a
code and a data descriptor whose base is the block and whose limit is 4 GiB,
reads each loadable segment from its own file through a 16 KiB transfer buffer
in conventional memory, and enters the executable at `e_entry` with EBX naming
the record. An address of the program is therefore an offset from the block;
any linear address L is reached at L minus the block's base.

`okdos seal <file>` (package `openkal-dos-tools`, the platform's tool) writes the
stub before an executable, in place; `okdos check <file>` reports whether a file
is such a program. The stub applies no relocation, which is why the executable
is linked at its address.

## The hand-over

The record (`src/dos.h`, version 1, 168 bytes) states the block's linear base,
its size and DPMI handle, the segment prefix's selector and segment, the
environment's segment, the transfer buffer's segment, size and selector, the
code and data selectors, the program's file (left open), and the name DOS
started the program by. `okdos_entry` (`src/start.S`) aligns the stack and calls
`okdos_start`, which refuses a record of another version with status 127.

## Two arrangements

| | entry | initialisers | `main` |
|---|---|---|---|
| beneath a C library (`standalone`) | `okdos_entry` records the hand-over and the arguments | the C library's, through `__libc_start_main` | called by the C library |
| alone (no C library) | `okdos_entry` | `__init_array`, run here | called here |

openkal-musl selects `standalone` on `cfg(os = "dos")`. A program that wants no
C library states `c-abi = false` for the row and calls openkal directly.

## What a program receives

| openkal | DOS |
|---|---|
| `stream` | DOS handles: 0, 1 and 2, and a file's (int 21h, 3Fh and 40h) through the transfer buffer; a handle's properties from IOCTL 4400h |
| `memory` | DPMI blocks (0501h, 0502h), 4 KiB granular; the free memory from 0500h |
| `env` | the arguments from the command tail (offset 80h of the segment prefix), the variables from the environment block, the program's name, the system name `dos` |
| `time` | the BIOS tick count at 0040:006C (54.925494 ms, made monotonic across midnight); the wall clock from int 21h 2Ah and 2Ch, read as UTC; a sleep by int 15h, 86h |
| `fs` | the current directory and each volume as supplied directories (`C:\WORK`, `C:`, `Z:`), a directory held as its name; open, create (5Bh refuses a name that exists), truncate, seek, transfers at an offset, find first and next for enquiry and listing, make and remove a directory, remove, rename, a file's time (5700h, 5701h) and a volume's capacity (36h) |
| `terminal` | the console's mode by bit 5 of its device word (cooked: line assembly and echo; raw: every byte as typed), ANSI.SYS's sequences where it answers int 2Fh 1A00h, the size from the BIOS data area |
| `exec` | a DPMI block, zeroed: the code descriptor spans 4 GiB, so every block is executable through it; publishing makes no request |
| `abort` | int 21h, 4Ch with the status; an abort writes its message to standard error and ends with 134 |

DOS is reached through DPMI's real-mode simulation (int 31h, 0300h), with every
buffer in the transfer buffer.

A volume is FAT's: names are eight characters, a dot and three, at most 64
bytes with the volume, and no case is distinguished; a node has a modification
time to two seconds and no identity; no link is made; DOS's rename refuses a
name that exists, which is the rename that does not replace
(`KAL_FS_PROP_RENAME_NO_REPLACE`), and the rename that replaces removes the name
first and is not atomic. A name that holds DOS's `\` or a volume's `:` is
refused, as a name that ascends is.

Absent (openkal clause 6.2): contexts, processes as openkal states them, the
network, and regions --- DOS cannot reserve an address apart from backing it: the
DPMI host CWSDPMI is DPMI 0.90 and refuses 0504h (measured), which is the case
`openkal/region.h` names for leaving the interface out. DOSBox Staging's console
accepts the request for raw mode and stays cooked (measured);
`kal_terminal_get_mode` reports what is in effect.

`examples/files` writes, appends and reads a file, renames it into a directory,
lists, resizes (the extension reads as zero) and removes them, and returns 9
(measured under DOSBox Staging).

## The row's conventions

The row compiles for `i386-unknown-none-elf` at `-march=i586` (the processor
the emulators that run DOS programs model), with emulated thread-local storage
(there is no thread register the toolchain can address: openkal-llvm-runtime
builds compiler-rt's `emutls.c` for the row) and unwind tables for every unit,
C included (clang's default for a triple with no system is none, and the
unwinder's own frames need them). The link is `ld.lld -static
--image-base=0x1000 -z norelro --gc-sections --eh-frame-hdr`, entered at
`okdos_entry`.

## CWSDPMI

CWSDPMI is Copyright (C) Charles W. Sandmann. It is not part of this package
and this package contains none of it: the stub runs `CWSDPMI.EXE` when it finds
it beside the program. CWSDPMI is distributed under its own license
(`cwsdpmi.doc` in csdpmi7b.zip, www.delorie.com/pub/djgpp/current/v2misc/),
which permits free redistribution of the unmodified binary with its notice. The
xim package `dosbox-staging`, which the row's runner uses, carries it under
those terms. A program distributed with `CWSDPMI.EXE` beside it carries that
notice too.

## License

Apache-2.0 (LICENSE).

[kal]: https://github.com/mcpplibs/openkal
[plat]: https://github.com/mcpplibs/openkal-platform
