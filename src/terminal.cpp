// openkal.terminal on DOS: the console device's mode (IOCTL 4400h and 4401h) and the display's
// size from the BIOS.
//
// DOS distinguishes one mode of the console, by bit 5 of its device word: cooked, where DOS
// assembles a line, echoes it and reserves Ctrl+C and Ctrl+S, and raw (binary), where each byte
// typed reaches the program as typed and nothing is reserved. The four input positions of openkal
// are therefore one switch here: cooked is KAL_TERM_LINE_EDIT | KAL_TERM_ECHO, raw is
// KAL_TERM_PASS_CONTROL | KAL_TERM_PASS_INPUT, and a requested mode is raw when it asks for no line
// assembly. KAL_TERM_SEQUENCES is reported where ANSI.SYS answers its installation check (int 2Fh,
// 1A00h). The size is the BIOS's: the columns at 0040:004A and the rows less one at 0040:0084.
#include <openkal/terminal.h>

#include "dos.h"

namespace {

using namespace okdos;

constexpr u32 kRaw = 0x20;

// The console's device word, or false for a handle that is not the console.
bool console_word(kal_stream s, u32& word) {
    if (s.h >= 256) return false;
    rm_regs r{};
    r.eax = 0x4400; r.ebx = static_cast<u32>(s.h);
    if (!dos(r)) return false;
    word = r.edx & 0xffff;
    return (word & 0x80) && (word & 0x03);
}

bool ansi_installed() {
    rm_regs r{};
    r.eax = 0x1a00;
    real_int(0x2f, r);
    return (r.eax & 0xff) == 0xff;
}

}  // namespace

extern "C" {

int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    u32 word = 0;
    if (!console_word(s, word)) return kal_err_not_supported;
    if (!mode) return kal_err_invalid;
    kal_uintptr m = (word & kRaw) ? (KAL_TERM_PASS_CONTROL | KAL_TERM_PASS_INPUT)
                                  : (KAL_TERM_LINE_EDIT | KAL_TERM_ECHO);
    if (ansi_installed()) m |= KAL_TERM_SEQUENCES;
    *mode = m;
    return kal_ok;
}

int kal_terminal_set_mode(kal_stream s, kal_uintptr mode) {
    u32 word = 0;
    if (!console_word(s, word)) return kal_err_not_supported;
    const bool raw = !(mode & KAL_TERM_LINE_EDIT);
    if (raw == ((word & kRaw) != 0)) return kal_ok;   // in effect already: not written again
    rm_regs r{};
    r.eax = 0x4401; r.ebx = static_cast<u32>(s.h);
    r.edx = (word & 0xff & ~kRaw) | (raw ? kRaw : 0);
    return dos(r) ? kal_ok : kal_err_io;
}

int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
    u32 word = 0;
    if (!console_word(s, word)) return kal_err_not_supported;
    const u32 c = *reinterpret_cast<volatile unsigned short*>(low(0x44a));
    const u32 r = *reinterpret_cast<volatile u8*>(low(0x484));
    if (c == 0) return kal_err_not_supported;
    if (cols) *cols = c;
    if (rows) *rows = r ? r + 1 : 25;
    return kal_ok;
}

kal_uintptr kal_terminal_props(kal_stream s) {
    u32 word = 0;
    if (!console_word(s, word)) return 0;
    return KAL_TERM_PROP_MODE | KAL_TERM_PROP_SIZE;
}

}  // extern "C"
