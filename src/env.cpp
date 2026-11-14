// openkal.env on DOS: the arguments are the command tail of the program's segment prefix (PSP),
// split at blanks; position zero is the name DOS started the program by; the variables are the
// environment block's NAME=VALUE strings.
#include <openkal/env.h>

#include "dos.h"

namespace {

using namespace okdos;

kal_uintptr length(const char* s) { kal_uintptr n = 0; while (s[n]) ++n; return n; }

kal_intptr give(const char* s, kal_uintptr n, char* out, kal_uintptr cap) {
    if (out && cap) {
        const kal_uintptr k = n < cap ? n : cap;
        for (kal_uintptr i = 0; i < k; ++i) out[i] = s[i];
    }
    return static_cast<kal_intptr>(n);
}

const char* environment() { return reinterpret_cast<const char*>(low(static_cast<u32>(self().h->env_segment) * 16u)); }

// The I-th NAME=VALUE string, or null.
const char* variable(kal_uintptr index) {
    const char* p = environment();
    for (kal_uintptr i = 0; *p; ++i) {
        if (i == index) return p;
        p += length(p) + 1;
    }
    return nullptr;
}

}  // namespace

extern "C" {

kal_uintptr kal_env_arg_count(void) { return static_cast<kal_uintptr>(self().argc); }

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    const state& s = self();
    if (index >= static_cast<kal_uintptr>(s.argc)) return -kal_err_not_found;
    return give(s.argv[index], length(s.argv[index]), out, cap);
}

kal_uintptr kal_env_var_count(void) {
    kal_uintptr n = 0;
    while (variable(n)) ++n;
    return n;
}

kal_intptr kal_env_var_at(kal_uintptr index, char* out, kal_uintptr cap) {
    const char* v = variable(index);
    if (!v) return -kal_err_not_found;
    kal_uintptr n = 0;
    while (v[n] && v[n] != '=') ++n;
    return give(v, n, out, cap);
}

kal_intptr kal_env_var(const char* name, kal_uintptr name_len, char* out, kal_uintptr cap) {
    for (kal_uintptr i = 0;; ++i) {
        const char* v = variable(i);
        if (!v) return -kal_err_not_found;
        kal_uintptr k = 0;
        while (k < name_len && v[k] && v[k] == name[k]) ++k;
        if (k == name_len && v[k] == '=') return give(v + k + 1, length(v + k + 1), out, cap);
    }
}

kal_intptr kal_env_image(char* out, kal_uintptr cap) {
    const char* n = self().h->name;
    return give(n, length(n), out, cap);
}

kal_intptr kal_env_loader(char*, kal_uintptr) { return -kal_err_not_found; }

kal_intptr kal_env_system(char* out, kal_uintptr cap) { return give("dos", 3, out, cap); }

int kal_env_resources(void) { return KAL_RESOURCES_ECONOMY; }

}  // extern "C"
