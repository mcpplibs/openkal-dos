// openkal.process on DOS (0.3.0): a program started by EXEC (int 21h, 4B00h), which returns when the
// started program has ended; its status is then int 21h, 4Dh's.
//
// DOS RUNS ONE PROGRAM. EXEC suspends the caller until the program it started ends, so
// kal_process_spawn returns after the end and kal_process_wait answers at once:
// KAL_PROCESS_PROP_RUNS_TO_END (clause 7.19). No running program remains to end or to speak to, so
// TERMINATE and CHANNEL are not claimed; a started program cannot outlive its starter, so
// BOUND_LIFETIME holds of every start and is claimed. The started program is a DOS program: one of
// this implementation enters protected mode through the DPMI host already resident, as a client
// of its own.
//
// WHAT THE STARTED PROGRAM RECEIVES (clause 7.13). Its streams are DOS handles 0, 1 and 2, each the
// caller's own, a stream the caller placed, or the device NUL; the handles AUX and PRN (3 and 4)
// that DOS gives every program; and no other handle: for the call, the caller's handle table
// (PSP:34h) holds the three positions and DOS's two devices and nothing else, and it is restored
// after. Its variables are an environment block of their own, in conventional memory (DPMI 0100h).
// Its directory is the one it was given, made DOS's current directory for the call. Its vector is
// the command tail, each element quoted where it holds a blank or a quote, by the convention DOS's C
// compilers and start.cpp divide a tail by; the first element travels in KAL_ARG0 (dos.h).
//
// THE BOUNDS ARE DOS'S (clause 7.6): a command tail of 126 bytes and an environment of 32 KiB. A
// start beyond either is kal_err_no_space; nothing is shortened.
#include <openkal/memory.h>
#include <openkal/process.h>
#include <openkal/time.h>

#include "dos.h"

namespace {

using namespace okdos;

u32 length(const char* s) { u32 n = 0; while (s[n]) ++n; return n; }

// A name in the transfer buffer at `at', for DS:DX.
void put_name(const char* name, u32 at) { copy(transfer() + at, name, length(name) + 1); }

// ── the programs started ────────────────────────────────────────────────────

struct started {
    bool used;
    int status;
    int terminated;
    kal_u64 id;
    kal_u64 started_ns;
};

started* g_started = nullptr;
kal_uintptr g_count = 0;
kal_uintptr g_cap = 0;
constexpr kal_uintptr kSelf = ~kal_uintptr{0};
kal_u64 g_starts = 0;

// A free row, the table doubled where there is none; zero where there is no memory.
kal_uintptr take_row() {
    for (kal_uintptr i = 0; i < g_count; ++i)
        if (!g_started[i].used) return i + 1;
    if (g_count == g_cap) {
        const kal_uintptr cap = g_cap ? 2 * g_cap : 16;
        auto* grown = static_cast<started*>(kal_alloc(cap * sizeof(started), alignof(started)));
        if (!grown) return 0;
        for (kal_uintptr i = 0; i < g_count; ++i) grown[i] = g_started[i];
        if (g_started) kal_free(g_started, g_cap * sizeof(started), alignof(started));
        g_started = grown;
        g_cap = cap;
    }
    g_started[g_count] = started{};
    return ++g_count;
}

started* row(kal_process p) {
    if (p.h == 0 || p.h > g_count) return nullptr;
    started* s = &g_started[p.h - 1];
    return s->used ? s : nullptr;
}

// ── the command tail ────────────────────────────────────────────────────────

constexpr u32 kTail = 126;   // the bytes of a tail before its carriage return

bool needs_quotes(const char* a, kal_uintptr n) {
    if (n == 0) return true;
    for (kal_uintptr i = 0; i < n; ++i)
        if (a[i] == ' ' || a[i] == '\t' || a[i] == '"') return true;
    return false;
}

// The tail after the first element: a blank, then each element, quoted where it must be ---
// a quote preceded by a backslash, and the backslashes before a quote doubled (start.cpp's
// division). A byte DOS ends a tail at, a carriage return or a NUL, cannot be carried.
int command_tail(const char** argv, const kal_uintptr* lens, kal_uintptr argc, bool one_string,
                 char* out, u32& n) {
    n = 0;
    const auto put = [&](char c) {
        if (n >= kTail) return false;
        out[n++] = c;
        return true;
    };
    for (kal_uintptr i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const kal_uintptr len = lens[i];
        for (kal_uintptr k = 0; k < len; ++k)
            if (a[k] == '\r' || a[k] == '\0') return kal_err_invalid;
        if (!put(' ')) return kal_err_no_space;
        const bool quote = !one_string && needs_quotes(a, len);
        if (quote && !put('"')) return kal_err_no_space;
        kal_uintptr slashes = 0;
        for (kal_uintptr k = 0; k < len; ++k) {
            if (a[k] == '\\') { ++slashes; if (!put('\\')) return kal_err_no_space; continue; }
            if (quote && a[k] == '"') {
                for (kal_uintptr s = 0; s <= slashes; ++s) if (!put('\\')) return kal_err_no_space;
            }
            slashes = 0;
            if (!put(a[k])) return kal_err_no_space;
        }
        if (quote) {
            for (kal_uintptr s = 0; s < slashes; ++s) if (!put('\\')) return kal_err_no_space;
            if (!put('"')) return kal_err_no_space;
        }
    }
    return kal_ok;
}

// ── the environment block ───────────────────────────────────────────────────

constexpr u32 kEnvironment = 32768;   // DOS's bound upon a program's environment

bool well_formed(const char* e, kal_uintptr n) {
    kal_uintptr k = 0;
    while (k < n && e[k] != '=') ++k;
    if (k == 0 || k == n) return false;
    for (kal_uintptr i = 0; i < n; ++i) if (e[i] == '\0') return false;
    return true;
}

constexpr char kResources[] = "KAL_RESOURCES=";
bool names_resources(const char* e, kal_uintptr n) {
    const u32 k = sizeof kResources - 1;
    if (n < k) return false;
    for (u32 i = 0; i < k; ++i) if (e[i] != kResources[i]) return false;
    return true;
}

// A block of conventional memory (DPMI 0100h): its segment and selector.
struct dos_block {
    u16 segment = 0, selector = 0;
    bool take(u32 bytes) {
        u32 ax = 0x0100, bx = (bytes + 15) / 16, dx = 0;
        int carry;
        __asm__ volatile("int $0x31\n\tsbb %3, %3" : "+a"(ax), "+b"(bx), "=d"(dx), "=r"(carry) : : "memory", "cc");
        if (carry) return false;
        segment = static_cast<u16>(ax);
        selector = static_cast<u16>(dx);
        return true;
    }
    ~dos_block() {
        if (!selector) return;
        u32 dx = selector;
        __asm__ volatile("int $0x31" : "+d"(dx) : "a"(0x0101) : "memory", "cc");
    }
};

// ── what the call changes, and restores ─────────────────────────────────────

// The caller's handle table: PSP:32h its size, PSP:34h its address.
u8* handle_table(u32& size) {
    const u8* psp = low(static_cast<u32>(self().h->psp_segment) * 16u);
    size = psp[0x32] | (psp[0x33] << 8);
    const u32 off = psp[0x34] | (psp[0x35] << 8), seg = psp[0x36] | (psp[0x37] << 8);
    return low(seg * 16u + off);
}

void close_handle(u16 h) {
    rm_regs r{};
    r.eax = 0x3e00; r.ebx = h;
    dos(r);
}

int exec_error(u32 code) {
    switch (code) {
        case 2: case 3: return kal_err_not_found;
        case 5:         return kal_err_permission;
        case 4: case 8: return kal_err_no_memory;
        case 0x0a:      return kal_err_no_space;    // an environment DOS does not take
        case 0x0b:      return kal_err_not_program;
        default:        return kal_err_io;
    }
}

constexpr kal_uintptr kKnownFlags = KAL_SPAWN_BOUND_LIFETIME | KAL_SPAWN_ONE_STRING | KAL_SPAWN_NULL_IN
    | KAL_SPAWN_NULL_OUT | KAL_SPAWN_NULL_ERR | KAL_SPAWN_RESOURCES_BALANCED
    | KAL_SPAWN_RESOURCES_PERFORMANCE | KAL_SPAWN_RESOURCES_ECONOMY;

// Offsets in the transfer buffer: the program's name, the tail, the parameter block, two FCBs.
constexpr u32 kAtName = 0, kAtTail = 128, kAtBlock = 260, kAtFcb1 = 280, kAtFcb2 = 320, kAtEnd = 360;

}  // namespace

extern "C" {

kal_uintptr kal_process_props(void) {
    return KAL_PROCESS_PROP_RUNS_TO_END | KAL_PROCESS_PROP_EXIT_STATUS | KAL_PROCESS_PROP_STREAM_PASSING
         | KAL_PROCESS_PROP_NULL_STREAMS | KAL_PROCESS_PROP_ONE_STRING | KAL_PROCESS_PROP_BOUND_LIFETIME;
}

int kal_process_spawn(const kal_spawn* how, const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams, kal_process* out) {
    if (!how || !path || path_len == 0 || !out || !self().h) return kal_err_invalid;
    if ((argc && (!argv || !argv_lens)) || (envc && (!envp || !envp_lens))) return kal_err_invalid;
    for (kal_uintptr i = 0; i < envc; ++i)
        if (!envp[i] || !well_formed(envp[i], envp_lens[i])) return kal_err_invalid;
    const kal_uintptr prefs = how->flags & (KAL_SPAWN_RESOURCES_BALANCED | KAL_SPAWN_RESOURCES_PERFORMANCE
                                            | KAL_SPAWN_RESOURCES_ECONOMY);
    if (prefs & (prefs - 1)) return kal_err_invalid;
    if (how->flags & ~kKnownFlags) return kal_err_not_supported;
    if (how->grants || how->job) return kal_err_not_supported;
    const kal_uintptr nulls[3] = {KAL_SPAWN_NULL_IN, KAL_SPAWN_NULL_OUT, KAL_SPAWN_NULL_ERR};
    const kal_uintptr placed[3] = {streams ? streams->in.h : 0, streams ? streams->out.h : 0,
                                   streams ? streams->err.h : 0};
    for (int i = 0; i < 3; ++i)
        if (placed[i] && (how->flags & nulls[i])) return kal_err_invalid;
    const char* work = directory_path(how->work);
    if (!work) return kal_err_invalid;

    // The program, named beneath `base': a start that cannot happen is reported by its reason
    // before DOS is asked.
    char file[kPathMax];
    if (const int e = program_path(how->base, path, path_len, file); e != kal_ok) return e;

    char tail[kTail];
    u32 tail_len = 0;
    if (const int e = command_tail(argv, argv_lens, argc, (how->flags & KAL_SPAWN_ONE_STRING) != 0, tail, tail_len);
        e != kal_ok)
        return e;

    // The variables: the caller's, with the preference in place of its KAL_RESOURCES where one is
    // asked for and the first element in place of any KAL_ARG0; two terminators end the block.
    const char* pref = prefs == KAL_SPAWN_RESOURCES_PERFORMANCE ? "KAL_RESOURCES=performance"
                     : prefs == KAL_SPAWN_RESOURCES_ECONOMY     ? "KAL_RESOURCES=economy"
                     : prefs == KAL_SPAWN_RESOURCES_BALANCED    ? "KAL_RESOURCES=balanced" : nullptr;
    const u32 file_len = length(file);
    const kal_uintptr arg0_len = argc ? argv_lens[0] : 0;
    for (kal_uintptr k = 0; k < arg0_len; ++k)
        if (argv[0][k] == '\0') return kal_err_invalid;
    unsigned long bytes = 2 + kArg0Len + file_len + 1 + arg0_len + 1 + (pref ? length(pref) + 1 : 0);
    for (kal_uintptr i = 0; i < envc; ++i) {
        if ((pref && names_resources(envp[i], envp_lens[i])) || (envp_lens[i] >= kArg0Len && names_arg0(envp[i])))
            continue;
        bytes += envp_lens[i] + 1;
    }
    if (bytes > kEnvironment) return kal_err_no_space;
    dos_block env;
    if (!env.take(static_cast<u32>(bytes))) return kal_err_no_memory;
    char* o = reinterpret_cast<char*>(low(static_cast<u32>(env.segment) * 16u));
    for (kal_uintptr i = 0; i < envc; ++i) {
        if ((pref && names_resources(envp[i], envp_lens[i])) || (envp_lens[i] >= kArg0Len && names_arg0(envp[i])))
            continue;
        copy(o, envp[i], static_cast<u32>(envp_lens[i]));
        o += envp_lens[i];
        *o++ = 0;
    }
    if (pref) { copy(o, pref, length(pref) + 1); o += length(pref) + 1; }
    if (argc) {
        copy(o, kArg0, kArg0Len); o += kArg0Len;
        copy(o, file, file_len); o += file_len;
        *o++ = '|';
        copy(o, argv[0], static_cast<u32>(arg0_len)); o += arg0_len;
        *o++ = 0;
    }
    *o++ = 0;
    *o = 0;

    const kal_uintptr index = take_row();
    if (!index) return kal_err_no_memory;

    // The streams: a handle of the caller's for each position, or NUL opened for it.
    u32 table_size = 0;
    u8* table = handle_table(table_size);
    if (table_size > 255) table_size = 255;
    u16 null_handle[3] = {0xffff, 0xffff, 0xffff};
    u8 entry[3];
    int failed = kal_ok;
    for (int i = 0; i < 3 && failed == kal_ok; ++i) {
        kal_uintptr h = placed[i];
        if (how->flags & nulls[i]) {
            put_name("NUL", kAtEnd);
            rm_regs r{};
            r.eax = i == 0 ? 0x3d00 : 0x3d01; r.edx = kAtEnd;
            if (!dos(r)) { failed = kal_err_io; break; }
            null_handle[i] = static_cast<u16>(r.eax & 0xffff);
            h = null_handle[i];
        }
        if (h == 0) h = static_cast<kal_uintptr>(i);
        if (h >= table_size || table[h] == 0xff) failed = kal_err_invalid;
        else entry[i] = table[h];
    }
    const auto close_nulls = [&] {
        for (u16 h : null_handle) if (h != 0xffff) close_handle(h);
    };
    if (failed != kal_ok) { close_nulls(); return failed; }

    // The directory it runs in: DOS's current drive and directory for the call.
    rm_regs drive{};
    drive.eax = 0x1900;
    dos(drive);
    const u32 old_drive = drive.eax & 0xff;
    char old_dir[68] = {static_cast<char>('A' + old_drive), ':', '\\', 0};
    rm_regs cwd{};
    cwd.eax = 0x4700; cwd.edx = 0; cwd.esi = kAtEnd;
    if (dos(cwd)) {
        const char* rest = reinterpret_cast<const char*>(transfer() + kAtEnd);
        u32 n = 0;
        while (rest[n] && n < 64) { old_dir[3 + n] = rest[n]; ++n; }
        old_dir[3 + n] = 0;
    }
    const auto enter = [&](const char* dir) {
        const char letter = dir[0] >= 'a' ? static_cast<char>(dir[0] - 32) : dir[0];
        rm_regs d{};
        d.eax = 0x0e00; d.edx = static_cast<u32>(letter - 'A');
        dos(d);
        char whole[kPathMax + 2];
        u32 n = length(dir);
        copy(whole, dir, n + 1);
        if (n == 2) { whole[2] = '\\'; whole[3] = 0; }
        put_name(whole, kAtEnd);
        rm_regs c{};
        c.eax = 0x3b00; c.edx = kAtEnd;
        return dos(c);
    };
    if (!enter(work)) { enter(old_dir); close_nulls(); return kal_err_not_found; }

    // The call: the name, the tail and the parameter block in the transfer buffer.
    u8* tb = transfer();
    for (u32 i = 0; i < kAtEnd; ++i) tb[i] = 0;
    put_name(file, kAtName);
    tb[kAtTail] = static_cast<u8>(tail_len);
    copy(tb + kAtTail + 1, tail, tail_len);
    tb[kAtTail + 1 + tail_len] = '\r';
    const u16 tbs = self().h->tb_segment;
    const auto word = [&](u32 at, u16 v) { tb[at] = static_cast<u8>(v); tb[at + 1] = static_cast<u8>(v >> 8); };
    word(kAtBlock, env.segment);
    word(kAtBlock + 2, kAtTail); word(kAtBlock + 4, tbs);
    word(kAtBlock + 6, kAtFcb1); word(kAtBlock + 8, tbs);
    word(kAtBlock + 10, kAtFcb2); word(kAtBlock + 12, tbs);

    u8 saved[256];
    for (u32 i = 0; i < table_size; ++i) saved[i] = table[i];
    for (u32 i = 0; i < table_size; ++i) table[i] = i < 3 ? entry[i] : i < 5 ? saved[i] : 0xff;
    const kal_u64 started_ns = kal_time_monotonic();
    rm_regs x{};
    x.eax = 0x4b00; x.edx = kAtName; x.ebx = kAtBlock;
    const bool ran = dos(x);
    for (u32 i = 0; i < table_size; ++i) table[i] = saved[i];
    enter(old_dir);
    close_nulls();
    if (!ran) return exec_error(x.eax & 0xffff);

    rm_regs end{};
    end.eax = 0x4d00;
    dos(end);
    started& r = g_started[index - 1];
    r = started{true, static_cast<int>(end.eax & 0xff), 0, ++g_starts, started_ns};
    // Ended by Ctrl-C (1) or by a critical error (2): an end the program did not choose.
    const u32 type = (end.eax >> 8) & 0xff;
    if (type == 1) { r.status = 130; r.terminated = 1; }
    else if (type == 2) r.terminated = 1;
    *out = kal_process{index};
    return kal_ok;
}

int kal_process_wait(kal_process p, int* status, int* terminated) {
    started* r = row(p);
    if (!r) return kal_err_invalid;
    if (status) *status = r->status;
    if (terminated) *terminated = r->terminated;
    return kal_ok;
}

int kal_process_terminate(kal_process p) { return row(p) ? kal_err_not_supported : kal_err_invalid; }

void kal_process_close(kal_process p) { if (started* r = row(p)) r->used = false; }

int kal_process_channel(kal_stream*, kal_stream*) { return kal_err_not_supported; }
void kal_process_channel_close(kal_stream) {}

const kal_u32* kal_process_stop_requested(void) { return nullptr; }
int kal_process_end_as_requested(void) { return kal_err_invalid; }

int kal_process_self(kal_process* out) {
    if (!out) return kal_err_invalid;
    *out = kal_process{kSelf};
    return kal_ok;
}

int kal_process_open_id(kal_u64, kal_process*) { return kal_err_not_supported; }

// WHAT IS KNOWN OF A PROGRAM: this program's identifier is its segment prefix's segment; a started
// program's is the count of starts (DOS gives it a segment prefix the starter never sees), with when
// it started on the monotonic clock and that it has ended.
int kal_process_info(kal_process p, kal_u32 wanted, kal_program_info* out) {
    if (!out || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    kal_program_info info{};
    info.self_size = out->self_size;
    if (p.h == kSelf) {
        info.id = self().h->psp_segment;
        info.present = KAL_PINFO_ID | KAL_PINFO_ENDED;
    } else if (const started* r = row(p)) {
        info.id = r->id;
        info.started_ns = r->started_ns;
        info.ended = 1;
        info.present = KAL_PINFO_ID | KAL_PINFO_STARTED | KAL_PINFO_ENDED;
    } else {
        return kal_err_invalid;
    }
    info.present &= wanted;
    const kal_uintptr n = out->self_size < sizeof info ? out->self_size : sizeof info;
    copy(out, &info, static_cast<u32>(n));
    return kal_ok;
}

int kal_process_job_enter(kal_job*) { return kal_err_not_supported; }
int kal_process_job_terminate(kal_job) { return kal_err_not_supported; }
void kal_process_job_close(kal_job) {}
int kal_process_job_create(kal_job*, char*, kal_uintptr, kal_uintptr*) { return kal_err_not_supported; }
int kal_process_job_open(const char*, kal_uintptr, kal_job*) { return kal_err_not_supported; }
int kal_process_job_observe(kal_job, const char*, kal_uintptr, kal_u32*) { return kal_err_not_supported; }
int kal_process_job_retire(kal_job, const char*, kal_uintptr, const kal_program_identity*, kal_u32*) {
    return kal_err_not_supported;
}
int kal_process_job_usage(kal_job, kal_job_usage*) { return kal_err_not_supported; }
int kal_process_job_limit(kal_job, kal_uintptr, kal_u64) { return kal_err_not_supported; }

}  // extern "C"
