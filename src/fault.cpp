// A fault of the program, reported as the other implementations of openkal report one: a sentence
// on standard error naming the fault and the instruction's address, and the status a process ended
// by the corresponding signal reports on the systems that have signals (128 and the signal's
// number). Without it, CWSDPMI writes a register dump to the screen and ends the program with
// status 1, which a program's output and status do not distinguish from `exit(1)` (measured).
//
// fault.S holds the handlers the DPMI host calls (int 31h, 0203h); they resume the program at
// okdos_fault on a stack of its own.
#include "dos.h"

extern "C" {
okdos::u16 okdos_fault_ds;
okdos::u32 okdos_fault_vector, okdos_fault_eip, okdos_fault_error;
void okdos_fault_0();
void okdos_fault_6();
void okdos_fault_12();
void okdos_fault_13();
void okdos_fault_14();
}

namespace okdos {
namespace {

struct kind {
    u32 vector;
    void (*handler)();
    const char* what;
    int status;   // 128 and the signal's number
};

const kind kKinds[] = {
    {0, okdos_fault_0, "a division by zero (#DE)", 128 + 8},
    {6, okdos_fault_6, "an instruction the processor does not have (#UD)", 128 + 4},
    {12, okdos_fault_12, "a fault of the stack (#SS)", 128 + 11},
    {13, okdos_fault_13, "a general protection fault (#GP)", 128 + 11},
    {14, okdos_fault_14, "an access to memory that is not there (#PF)", 128 + 11},
};

unsigned put(char* out, const char* s) {
    unsigned n = 0;
    while (s[n]) { out[n] = s[n]; ++n; }
    return n;
}

unsigned hex(char* out, u32 v) {
    const char digits[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; ++i) out[2 + i] = digits[(v >> (28 - 4 * i)) & 15];
    return 10;
}

}  // namespace

void install_faults() {
    u16 cs = 0, ds = 0;
    __asm__ volatile("movw %%cs, %0\n\tmovw %%ds, %1" : "=r"(cs), "=r"(ds));
    okdos_fault_ds = ds;
    for (const kind& k : kKinds) {
        u32 ax = 0x0203;
        __asm__ volatile("int $0x31"
                         : "+a"(ax)
                         : "b"(k.vector), "c"(static_cast<u32>(cs)),
                           "d"(reinterpret_cast<unsigned long>(k.handler))
                         : "memory", "cc");
    }
}

}  // namespace okdos

extern "C" [[noreturn]] void okdos_fault() {
    using namespace okdos;
    const kind* k = &kKinds[0];
    for (const kind& c : kKinds)
        if (c.vector == okdos_fault_vector) k = &c;
    char text[160];
    unsigned n = put(text, "openkal-dos: fault: ");
    n += put(text + n, k->what);
    n += put(text + n, " at ");
    n += hex(text + n, okdos_fault_eip);
    if (okdos_fault_vector >= 12) {
        n += put(text + n, " (error ");
        n += hex(text + n, okdos_fault_error);
        n += put(text + n, ")");
    }
    n += put(text + n, "\r\n");
    write_handle(2, reinterpret_cast<const u8*>(text), n);
    __asm__ volatile("int $0x21" ::"a"(0x4c00 | (k->status & 0xff)));
    for (;;) {}
}
