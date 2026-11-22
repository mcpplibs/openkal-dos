// What the stub hands over, and the two ways this implementation reaches DOS.
//
// THE HAND-OVER (stub/stub.S writes it, version 2). The program is entered in 32-bit protected
// mode with CS, DS, ES and SS descriptors whose base is the block the stub allocated and whose
// limit spans 4 GiB, so an address of the program is an offset from the block's base and any
// linear address L is reached at L - base. EBX holds the record's offset.
//
// THE BLOCK is [loadable segments][the record's page][the guard page][the stack]; the stack's top
// is the block's end.
#pragma once
#include <openkal/fs.h>

namespace okdos {

using u8 = unsigned char; using u16 = unsigned short; using u32 = unsigned int;

struct handoff {
    u32  magic;            // "OKDS"
    u32  version;          // 2
    u32  base;             // the block's linear address
    u32  size;             // its size
    u32  block;            // its DPMI handle
    u16  psp_selector;
    u16  psp_segment;
    u16  env_segment;      // the environment block, in conventional memory
    u16  tb_segment;       // the transfer buffer: conventional memory DOS reads and writes
    u32  tb_size;
    u16  code_selector, data_selector, tb_selector;
    u16  exe_handle;       // the program's own file, opened by the stub
    char name[128];        // the program's name as DOS gave it
    // Version 2 (0.1.1). A record of version 1 (the 0.1.0 stub) ends here; its program has the
    // stack of 64 KiB above the record and no guard page.
    u32  guard;            // the guard page's offset in the block
    u32  stack_size;       // the stack's size: PT_GNU_STACK's, or the stub's default (1 MiB)
};
static_assert(sizeof(handoff) == 176);
constexpr u32 kVersion = 2;
constexpr u32 kMagic = 0x53444b4fu;

// DPMI's real-mode call structure (int 31h, 0300h).
struct rm_regs {
    u32 edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    u16 flags, es, ds, fs, gs, ip, cs, sp, ss;
} __attribute__((packed));
static_assert(sizeof(rm_regs) == 50);

struct state {
    const handoff* h = nullptr;
    int argc = 0;
    char** argv = nullptr;
    u32 ticks_base = 0, ticks_last = 0, ticks_days = 0;   // the BIOS tick count, made monotonic
};
state& self();

// A real-mode interrupt with these registers (DPMI 0300h); its carry is the result's.
// Interrupts are enabled during it: DOS may wait on them.
bool real_int(u8 vector, rm_regs& r);
// int 21h with registers only, no buffers; false on carry.
bool dos(rm_regs& r);

// A linear address of conventional memory, as this program reaches it.
inline u8* low(u32 linear) { return reinterpret_cast<u8*>(static_cast<unsigned long>(linear - self().h->base)); }
inline u8* transfer() { return low(static_cast<u32>(self().h->tb_segment) * 16u); }
inline u32 transfer_size() { return self().h->tb_size; }

void copy(void* to, const void* from, u32 n);

// A handle's transfers through the transfer buffer (int 21h, 3Fh and 40h): a write of the whole
// buffer a piece at a time, a read of at most the transfer buffer's size.
long write_handle(u16 handle, const u8* p, unsigned long n);
long read_handle(u16 handle, u8* p, unsigned long n);
// Whether a file's handle was opened for appending (fs.cpp): each write is placed at the end.
bool append_handle(u16 handle);
// The handlers of the processor's faults (fault.S, fault.cpp): a fault is reported on standard
// error and ends the program with 128 and the signal's number.
void install_faults();
// The guard page below the stack (stack.cpp): uncommitted through DPMI 1.0 (int 31h, 0507h), so an
// overflow is a page fault; where the host refuses, a canary checked at the program's end.
void guard_stack();
void check_stack();

// openkal.process (process.cpp, 0.3.0) and the volume (fs.cpp): the whole name of a program beneath
// a directory, in `out' (kPathMax bytes), or the reason there is none; a directory's whole name.
constexpr u32 kPathMax = 128;
int program_path(kal_dir base, const char* name, kal_uintptr len, char* out);
const char* directory_path(kal_dir d);

// THE NAME A STARTED PROGRAM OBSERVES AS ITS OWN (0.3.0, clause 7.6). DOS gives a program no first
// element: the name EXEC was given is written after its environment, and a program finds its file
// by it. A starter of this implementation writes `KAL_ARG0=<that name>|<the first element>' among
// the variables; the program it starts reads the first element there where the name is its own,
// and a variable inherited through a program that does not read it names another program's file.
// The variable is the implementation's and is not among the variables a program enumerates.
inline constexpr char kArg0[] = "KAL_ARG0=";
inline constexpr u32 kArg0Len = sizeof kArg0 - 1;
inline bool names_arg0(const char* e) {
    for (u32 i = 0; i < kArg0Len; ++i) if (e[i] != kArg0[i]) return false;
    return true;
}

// Days from 1970-01-01 to a civil date (H. Hinnant's algorithm).
inline long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

}  // namespace okdos
