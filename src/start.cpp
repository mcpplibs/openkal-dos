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

u32 length(const char* s) { u32 n = 0; while (s[n]) ++n; return n; }

bool same_name(const char* a, const char* b, u32 n) {
    const auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    for (u32 i = 0; i < n; ++i) if (up(a[i]) != up(b[i])) return false;
    return true;
}

// THE FIRST ELEMENT (0.3.0): the starter's, from KAL_ARG0 where it names this program's file
// (dos.h); the name DOS started the program by otherwise.
const char* first_element(const handoff* h) {
    const char* e = reinterpret_cast<const char*>(low(static_cast<u32>(h->env_segment) * 16u));
    for (; *e; e += length(e) + 1) {
        if (!names_arg0(e)) continue;
        const char* v = e + kArg0Len;
        u32 k = 0;
        while (v[k] && v[k] != '|') ++k;
        if (v[k] == '|' && k == length(h->name) && same_name(v, h->name, k)) return v + k + 1;
    }
    return h->name;
}

// THE TAIL IS DIVIDED AS DOS'S C COMPILERS DIVIDE IT (0.3.0): blanks separate; a double quote
// begins or ends a part in which they do not; backslashes before a quote are halved, and an odd
// one makes the quote a character; other backslashes are themselves. 0.2.0 divided at blanks
// alone, so `"a b"' arrived as two arguments with their quotes, and no starter could pass an
// argument holding a blank. The words are written over the tail, which is never shorter.
void record_arguments(const handoff* h) {
    state& s = self();
    const u8* psp = low(static_cast<u32>(h->psp_segment) * 16u);
    u32 n = psp[0x80];
    if (n > 127) n = 127;
    copy(tail, psp + 0x81, n);
    tail[n] = 0;
    for (u32 i = 0; i < n; ++i) if (tail[i] == '\r') { n = i; break; }
    int argc = 0;
    words[argc++] = const_cast<char*>(first_element(h));
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    u32 i = 0, o = 0;
    while (i < n) {
        while (i < n && blank(tail[i])) ++i;
        if (i >= n) break;
        char* word = &tail[o];
        bool quoted = false;
        while (i < n && (quoted || !blank(tail[i]))) {
            if (tail[i] == '\\') {
                u32 k = 0;
                while (i < n && tail[i] == '\\') { ++k; ++i; }
                const bool quote = i < n && tail[i] == '"';
                for (u32 b = 0; b < (quote ? k / 2 : k); ++b) tail[o++] = '\\';
                if (quote && (k & 1)) { tail[o++] = '"'; ++i; }
                continue;
            }
            if (tail[i] == '"') { quoted = !quoted; ++i; continue; }
            tail[o++] = tail[i++];
        }
        // The terminator: over the separator just passed, or the byte after the tail.
        tail[o++] = 0;
        if (o > i) i = o;
        words[argc++] = word;
    }
    words[argc] = nullptr;
    s.argc = argc;
    s.argv = words;
}

}  // namespace

extern "C" [[noreturn]] void okdos_start(const handoff* h) {
    if (h->magic != kMagic || h->version < 1 || h->version > kVersion) {
        // A stub of another version: nothing here can be trusted to say so but the status.
        kal_exit(127);
    }
    self().h = h;
    install_faults();
    guard_stack();
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
