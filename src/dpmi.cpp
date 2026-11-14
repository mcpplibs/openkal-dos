// DOS through DPMI: a real-mode interrupt simulated by the host (int 31h, 0300h), with buffers in
// the transfer buffer the stub allocated in conventional memory.
#include "dos.h"

namespace okdos {

state& self() {
    static state s{};
    return s;
}

void copy(void* to, const void* from, u32 n) {
    auto* d = static_cast<u8*>(to);
    const auto* s = static_cast<const u8*>(from);
    while (n--) *d++ = *s++;
}

bool real_int(u8 vector, rm_regs& r) {
    r.flags = 0x0202;   // interrupts enabled
    r.ss = 0;           // the host's stack
    r.sp = 0;
    int carry;
    __asm__ volatile("int $0x31\n\tsbb %0, %0"
                     : "=r"(carry)
                     : "a"(0x0300), "b"(static_cast<u32>(vector)), "c"(0), "D"(&r)
                     : "memory", "cc");
    return carry == 0 && (r.flags & 1) == 0;
}

bool dos(rm_regs& r) {
    if (r.ds == 0) r.ds = self().h->tb_segment;
    if (r.es == 0) r.es = self().h->tb_segment;
    return real_int(0x21, r);
}

}  // namespace okdos

// A compiler zeroes and copies a structure through these two whether or not a C library is
// linked; without one they are these, and a C library's own replace them (weak).
extern "C" {
__attribute__((weak)) void* memset(void* p, int c, unsigned long n) {
    auto* d = static_cast<unsigned char*>(p);
    while (n--) *d++ = static_cast<unsigned char>(c);
    return p;
}
__attribute__((weak)) void* memcpy(void* to, const void* from, unsigned long n) {
    okdos::copy(to, from, static_cast<okdos::u32>(n));
    return to;
}
}
