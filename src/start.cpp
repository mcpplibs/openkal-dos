// What the stub enters, and what a program receives (openkal clause 7.13).
//
// okdos_entry (start.S) calls okdos_start with the stub's hand-over record. The arguments are the
// command tail of the program's segment prefix, at offset 80h: a count, then the text, ended by
// a carriage return; position zero is the program's name.
#include <openkal/abort.h>

#include "dos.h"

extern "C" int main(int, char**, char**);

#ifdef OKDOS_STANDALONE
// THE STANDALONE ARRANGEMENT: a C library above this implementation (openkal-musl names it so
// on `cfg(os = "dos")') receives the program at the name every C library's hand-over has; it
// obtains the arguments through openkal.env and runs the initialisers itself.
extern "C" int __libc_start_main(int (*)(int, char**, char**), int, char**,
                                 void (*)(), void (*)(), void (*)());
#else
// Without a C library, the initialisers the linker gathered run here, in order.
extern "C" {
using initialiser = void (*)();
extern initialiser __init_array_start[] __attribute__((weak));
extern initialiser __init_array_end[] __attribute__((weak));
}
#endif

namespace {

using namespace okdos;

// Room for the command tail's words: at most 127 bytes, so at most 64 words.
char tail[128];
char* words[66];

void record_arguments(const handoff* h) {
    state& s = self();
    const u8* psp = low(static_cast<u32>(h->psp_segment) * 16u);
    u32 n = psp[0x80];
    if (n > 127) n = 127;
    copy(tail, psp + 0x81, n);
    tail[n] = 0;
    int argc = 0;
    words[argc++] = const_cast<char*>(h->name);
    for (u32 i = 0; i < n;) {
        while (i < n && (tail[i] == ' ' || tail[i] == '\t' || tail[i] == '\r')) tail[i++] = 0;
        if (i >= n) break;
        words[argc++] = &tail[i];
        while (i < n && tail[i] != ' ' && tail[i] != '\t' && tail[i] != '\r') ++i;
    }
    words[argc] = nullptr;
    s.argc = argc;
    s.argv = words;
}

}  // namespace

extern "C" [[noreturn]] void okdos_start(const handoff* h) {
    if (h->magic != kMagic || h->version != 1) {
        // A stub of another version: nothing here can be trusted to say so but the status.
        kal_exit(127);
    }
    self().h = h;
    record_arguments(h);
#ifdef OKDOS_STANDALONE
    __libc_start_main(main, 0, nullptr, nullptr, nullptr, nullptr);
    kal_exit(127);
#else
    if (__init_array_start)
        for (initialiser* f = __init_array_start; f < __init_array_end; ++f) (*f)();
    static char* no_environment[] = {nullptr};
    kal_exit(main(self().argc, self().argv, no_environment));
#endif
}
