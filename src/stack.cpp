// The guard page below the stack (openkal-dos 0.1.1).
//
// The stub reserves one page between the hand-over record's page and the stack (src/dos.h). Made
// uncommitted through DPMI 1.0's 0507h (set page attributes), it turns a stack that grows past its
// size into a page fault, which fault.cpp reports with status 139. A host without 0507h leaves the
// page committed: the page is then filled with a canary, a sentence on standard error says at the
// start that the guard is absent, and the program's end checks the canary.
#include "dos.h"

namespace okdos {
namespace {

constexpr u32 kPage = 4096;
constexpr u32 kCanary = 0x4b415453u;   // "STAK"
bool canary_set = false;

u32* guard_words() {
    return reinterpret_cast<u32*>(static_cast<unsigned long>(self().h->guard));
}

void say(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    write_handle(2, reinterpret_cast<const u8*>(s), n);
}

}  // namespace

void guard_stack() {
    const handoff* h = self().h;
    if (h->version < 2) return;   // the 0.1.0 stub reserves no guard page
    u16 attribute = 0;   // uncommitted
    u32 ax = 0x0507, pages = 1;   // ECX returns the pages set on a refusal
    int carry;
    __asm__ volatile("int $0x31\n\tsbb %2, %2"
                     : "+a"(ax), "+c"(pages), "=r"(carry)
                     : "S"(h->block), "b"(h->guard), "d"(&attribute)
                     : "memory", "cc");
    if (carry == 0) return;
    u32* w = guard_words();
    for (u32 i = 0; i < kPage / 4; ++i) w[i] = kCanary;
    canary_set = true;
    say("openkal-dos: the DPMI host has no 0507h: no guard page below the stack; an overflow is "
        "reported at the program's end\r\n");
}

void check_stack() {
    if (!canary_set) return;
    const u32* w = guard_words();
    for (u32 i = 0; i < kPage / 4; ++i)
        if (w[i] != kCanary) {
            canary_set = false;
            say("openkal-dos: the stack overflowed its size\r\n");
            __asm__ volatile("int $0x21" ::"a"(0x4c00 | (128 + 11)));
            for (;;) {}
        }
}

}  // namespace okdos
