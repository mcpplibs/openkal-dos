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
| the stub (`stub/stub.S`, 2656 bytes) | an MZ program in 16-bit code |
| the executable | ELF32, i386, linked at its address (`ET_EXEC`, base 0x1000), at most 16 program headers |

The stub finds a 32-bit DPMI host (int 2Fh, 1687h), and where there is none it
runs `CWSDPMI.EXE` from the program's own directory and asks again. It enters
protected mode as a 32-bit client, allocates one block for the executable's
loadable segments, the hand-over record, a guard page and the stack, makes a
code and a data descriptor whose base is the block and whose limit is 4 GiB,
reads each loadable segment from its own file through a 16 KiB transfer buffer
in conventional memory, and enters the executable at `e_entry` with EBX naming
the record. An address of the program is therefore an offset from the block;
any linear address L is reached at L minus the block's base.

`okdos seal <file>` (package `openkal-dos-tools`, the platform's tool) writes the
stub before an executable, in place; `okdos check <file>` reports whether a file
is such a program. The stub applies no relocation, which is why the executable
is linked at its address.

## The stack

The block is `[loadable segments][the record's page][the guard page][the
stack]`, and the stack's top is the block's end. The stack's size is a property
of the executable, fixed at its link: the program header `PT_GNU_STACK`'s
`p_memsz` (lld's `-z stack-size=N`), rounded up to a page. Where the header is
absent or states zero, the stub's default applies, 1 MiB. A program states
another size with its own link flags; the row links with `ld.lld` itself, so
the flag is the linker's:

```toml
[build]
ldflags = ["-z stack-size=4194304"]
```

The runtime makes the guard page uncommitted through DPMI 1.0's 0507h (set page
attributes), so a stack that grows past its size is a page fault, reported as
every fault is (status 139). CWSDPMI r7 under DOSBox Staging grants 0507h
(measured: the record's page below the guard page reads, the guard page
faults). A host that refuses it leaves the page committed: the runtime fills it
with a canary, says on standard error at the start that the guard is absent, and
checks the canary at the program's end, where an overwritten canary ends the
program with a sentence and status 139.

## The hand-over

The record (`src/dos.h`, version 2, 176 bytes) states the block's linear base,
its size and DPMI handle, the segment prefix's selector and segment, the
environment's segment, the transfer buffer's segment, size and selector, the
code and data selectors, the program's file (left open), the name DOS started
the program by, the guard page's offset and the stack's size. `okdos_entry`
(`src/start.S`) aligns the stack and calls `okdos_start`, which accepts versions
1 and 2 and refuses any other with status 127. A record of version 1 (the 0.1.0
stub) has no guard page, and its program keeps the 64 KiB stack of that stub.

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
| (faults) | a processor fault (division by zero, an instruction the processor lacks, the stack, general protection, a page) is reported on standard error with the instruction's address, and the program ends with 128 and the corresponding signal's number (136, 132, 139), through DPMI's exception handlers (int 31h, 0203h); CWSDPMI's own report is a register dump on the screen and status 1 (measured) |

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

0.2.0 is written against openkal 0.26, and nothing but the specification's
version changes; `openkal-dos-tools` carries the same version, as the two are one
archive. What 0.26 requires is of `openkal.process`, which is absent here: a
start bounded by the environment alone (clause 7.6) and a unit claimed only
where a reference can be made (clause 7.18). What a program receives is bounded
by DOS alone already: the command tail of the segment prefix holds at most 127
bytes, the room `src/start.cpp` keeps for its words is that bound's, and the
variables are read from the environment block where it lies, however many it
holds.

An i386 ok image (`i686-openkal`) runs on DOS as on firmware: the platform's
runner `hosts` include `dos-i686`, so `mcpp pack --pkg-format single-file
--systems dos-i686` builds openkal-run's core runner as a program of this row,
with the image in its `.oke` section, and the runner places the image through
`exec`. Measured under DOSBox Staging: an image built for `i686-openkal`
(C++23, `import std`, an exception across a frame) sorted its arguments, read
`PATH` and returned 7.

`examples/files` writes, appends and reads a file, renames it into a directory,
lists, resizes (the extension reads as zero) and removes them, and returns 9
(measured under DOSBox Staging).

## The row's conventions

The row compiles for `i386-unknown-none-elf` at `-march=i486`: DOSBox Staging
0.83 executes RDTSC and not CMPXCHG8B, under every `cputype` (measured), so a
sixty-four bit atomic is compiler-rt's call (`atomic.c`) rather than that
instruction. The row also uses emulated thread-local storage
(there is no thread register the toolchain can address: openkal-llvm-runtime
builds compiler-rt's `emutls.c` for the row) and unwind tables for every unit,
C included (clang's default for a triple with no system is none, and the
unwinder's own frames need them), and a stack aligned to sixteen bytes at a
call, as an i386 ok image's is. The link is `ld.lld -static
--image-base=0x1000 -z norelro --gc-sections --eh-frame-hdr`, entered at
`okdos_entry`.

## Checks

CI (`.github/workflows/ci.yml`) asserts that `stub/stub.S` assembles to the
committed `tools/src/stub_bytes.h`; that `okdos` builds on Linux, macOS and
Windows and refuses a file that is no ELF32 i386 executable; and that the
examples and the tests, built for `i686-dos-musl` with this repository's
openkal-dos and seal and run under DOSBox Staging, print what they state and end
with their statuses: `examples/hello` 7 (unoptimised and optimised),
`examples/files` 9, `tests/fault` 139, `tests/stack` 0 (sixty-four frames of
8 KiB within the default stack) and `tests/stack-small` 139 (the same recursion
with a stack of 64 KiB, stopped by the guard page).

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
