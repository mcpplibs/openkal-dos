// openkal.fs on DOS (int 21h), through the transfer buffer.
//
// A DIRECTORY IS A NAME. DOS has no handle for a directory, so a kal_dir is a slot holding the
// directory's whole name (`C:\WORK'), and every operation joins it with the name it is given,
// each `/' written as DOS's `\'. The supplied directories are those of a Windows environment:
// the current directory first (`C:\WORK'), the one the program was started in, then each volume
// DOS has (`C:', `Z:'). A C library above openkal finds the longest supplied name that is a prefix
// of an absolute name, and a volume's name compares as DOS compares it.
//
// THE VOLUME'S PROPERTIES ARE FAT'S: no case is distinguished, no link is made, a node has a
// modification time (to two seconds, DOS's local time read as UTC, as openkal.time reads the
// clock) and no identity, and a rename refuses a name that exists, which is the rename that does
// not replace; the rename that replaces removes the name first, and is not atomic. A volume's
// capacity is int 21h, 36h's. A listing gives each entry's size and time (find first and next).
//
// The names are DOS's: eight characters, a dot and three, at most 64 bytes in all with the
// volume; a longer whole name is refused by DOS and reported as not found.
#include <openkal/fs.h>
#include <openkal/memory.h>
#include <openkal/stream.h>

#include "dos.h"

namespace {

using namespace okdos;

constexpr int kDirs = 32;
constexpr u32 kPath = 128;
constexpr kal_uintptr kMaxName = 64;

// THE TABLES ARE ZERO BEFORE THE PROGRAM STARTS (no initialiser runs for them): a C library above
// reads the supplied directories during its own start, before the program's initialisers run, and
// an initialiser here would then empty the table it had filled.
struct listing {
    bool active;
    bool first;
    u8   dta[43];
};

struct directory {
    bool    used;
    char    path[kPath];   // `C:\', `C:\WORK'
    listing list;
};

directory g_dirs[kDirs];
int g_preopens = -1;
int g_preopen_slot[kDirs];
bool g_append[256];
bool g_writable[256];

u32 length(const char* s) { u32 n = 0; while (s[n]) ++n; return n; }

directory* dir_of(kal_dir d) {
    if (d.h == 0 || d.h > kDirs || !g_dirs[d.h - 1].used) return nullptr;
    return &g_dirs[d.h - 1];
}

int new_dir(const char* path) {
    for (int i = 0; i < kDirs; ++i)
        if (!g_dirs[i].used) {
            g_dirs[i].used = true;
            g_dirs[i].list.active = false;
            const u32 n = length(path);
            copy(g_dirs[i].path, path, n + 1);
            return i;
        }
    return -1;
}

int error_of(const rm_regs& r) {
    switch (r.eax & 0xffff) {
        case 2: case 3: case 0x0f: case 0x12: return kal_err_not_found;
        case 4: case 8: return kal_err_no_memory;
        case 5: return kal_err_permission;
        case 6: return kal_err_invalid;
        case 0x10: return kal_err_permission;   // the current directory is not removed
        case 0x11: return kal_err_invalid;      // not the same volume
        case 0x50: return kal_err_exists;
        default: return kal_err_io;
    }
}

// The whole name of `name' beneath `base', in `out'; `.' is the directory itself. A name that
// ascends or starts at a root is refused, as the interface states.
int join(kal_dir base, const char* name, kal_uintptr len, char* out) {
    const directory* d = dir_of(base);
    if (!d) return kal_err_invalid;
    if (!name || len == 0 || name[0] == '/') return kal_err_invalid;
    if (len > kMaxName) return kal_err_invalid;
    u32 o = length(d->path);
    copy(out, d->path, o);
    if (len == 1 && name[0] == '.') { out[o] = 0; return kal_ok; }
    if (o && out[o - 1] != '\\') out[o++] = '\\';
    for (kal_uintptr i = 0; i < len;) {
        kal_uintptr j = i;
        while (j < len && name[j] != '/') ++j;
        const kal_uintptr n = j - i;
        if (n == 2 && name[i] == '.' && name[i + 1] == '.') return kal_err_invalid;
        if (!(n == 1 && name[i] == '.') && n) {
            if (o + n + 2 >= kPath) return kal_err_invalid;
            if (o && out[o - 1] != '\\') out[o++] = '\\';
            for (kal_uintptr k = 0; k < n; ++k) {
                // DOS's own separator and a volume's colon would leave the directory.
                if (name[i + k] == '\\' || name[i + k] == ':') return kal_err_invalid;
                out[o++] = name[i + k];
            }
        }
        i = j + 1;
    }
    if (o > 3 && out[o - 1] == '\\') --o;
    out[o] = 0;
    return kal_ok;
}

// A name in the transfer buffer at `at', for DS:DX (or ES:DI).
void put_name(const char* path, u32 at) {
    copy(transfer() + at, path, length(path) + 1);
}

// A volume's root: `C:\'.
bool is_root(const char* path) { return path[1] == ':' && path[2] == '\\' && path[3] == 0; }

// int 21h, 4300h: the attributes of a name; -1 when it does not exist.
int attributes(const char* path) {
    if (is_root(path)) return 0x10;
    put_name(path, 0);
    rm_regs r{};
    r.eax = 0x4300; r.edx = 0;
    if (!dos(r)) return -1;
    return static_cast<int>(r.ecx & 0xffff);
}

kal_u64 dos_time_ns(u32 date, u32 time) {
    const long long y = 1980 + ((date >> 9) & 0x7f);
    const unsigned m = (date >> 5) & 0x0f, d = date & 0x1f;
    if (m == 0 || d == 0) return 0;
    const long long s = days_from_civil(y, m, d) * 86400 + ((time >> 11) & 0x1f) * 3600
                      + ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;
    return s < 0 ? 0 : static_cast<kal_u64>(s) * 1000000000ull;
}

// DOS's date and time words of a wall time; false before 1980 or after 2107.
bool ns_dos_time(kal_u64 ns, u32& date, u32& time) {
    long long s = static_cast<long long>(ns / 1000000000ull);
    long long days = s / 86400;
    long long rem = s % 86400;
    // civil_from_days (H. Hinnant)
    days += 719468;
    const long long era = days / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y < 1980 || y > 2107) return false;
    date = static_cast<u32>(((y - 1980) << 9) | (m << 5) | d);
    time = static_cast<u32>(((rem / 3600) << 11) | (((rem % 3600) / 60) << 5) | ((rem % 60) / 2));
    return true;
}

void fill(kal_node_info* out, int kind, kal_u32 wanted, bool sized, kal_u64 size, bool timed, kal_u64 ns,
          int attr) {
    const kal_u32 cap = out->self_size;
    kal_node_info v{};
    v.self_size = cap;
    v.kind = kind;
    v.present = KAL_INFO_KIND;
    if (sized && (wanted & KAL_INFO_SIZE)) { v.size = size; v.present |= KAL_INFO_SIZE; }
    if (timed && (wanted & KAL_INFO_MODIFIED)) { v.modified_ns = ns; v.present |= KAL_INFO_MODIFIED; }
    if (attr >= 0 && (wanted & KAL_INFO_WRITABLE)) { v.writable = (attr & 1) == 0; v.present |= KAL_INFO_WRITABLE; }
    const u32 n = cap < sizeof v ? cap : static_cast<u32>(sizeof v);
    copy(out, &v, n);
    out->self_size = cap;
}

// Find first (4Eh) for `path' into the transfer buffer's DTA at offset 512; true when found.
bool find_first(const char* path, u16 attrs) {
    rm_regs set{};
    set.eax = 0x1a00; set.edx = 512;
    dos(set);
    put_name(path, 0);
    rm_regs r{};
    r.eax = 0x4e00; r.ecx = attrs; r.edx = 0;
    return dos(r);
}

u32 dta_u16(const u8* dta, u32 at) { return dta[at] | (dta[at + 1] << 8); }
u32 dta_u32(const u8* dta, u32 at) { return dta_u16(dta, at) | (dta_u16(dta, at + 2) << 16); }

void preopen_table() {
    if (g_preopens >= 0) return;
    g_preopens = 0;
    rm_regs drive{};
    drive.eax = 0x1900;
    dos(drive);
    const char letter = static_cast<char>('A' + (drive.eax & 0xff));
    // The current directory: 47h writes it without the volume and the first separator.
    rm_regs cwd{};
    cwd.eax = 0x4700; cwd.edx = 0; cwd.esi = 0;
    char path[kPath] = {letter, ':', '\\', 0};
    if (dos(cwd)) {
        const char* rest = reinterpret_cast<const char*>(transfer());
        u32 n = 0;
        while (rest[n] && n < 64) { path[3 + n] = rest[n]; ++n; }
        path[3 + n] = 0;
    }
    const int s = new_dir(path);
    if (s >= 0) g_preopen_slot[g_preopens++] = s;
    // The volumes: IOCTL 4409h answers for a drive that exists.
    for (int v = 0; v < 26 && g_preopens < kDirs / 2; ++v) {
        rm_regs r{};
        r.eax = 0x4409; r.ebx = static_cast<u32>(v + 1);
        if (!dos(r)) continue;
        const char root[4] = {static_cast<char>('A' + v), ':', '\\', 0};
        const int slot = new_dir(root);
        if (slot >= 0) g_preopen_slot[g_preopens++] = slot;
    }
}

}  // namespace

namespace okdos {
bool append_handle(u16 handle) { return handle < 256 && g_append[handle]; }
}

extern "C" {

kal_uintptr kal_fs_preopen_count(void) {
    preopen_table();
    return static_cast<kal_uintptr>(g_preopens);
}

int kal_fs_preopen(kal_uintptr index, kal_dir* out, char* name_out, kal_uintptr name_cap,
                   kal_uintptr* name_len) {
    preopen_table();
    if (!out || index >= static_cast<kal_uintptr>(g_preopens)) return kal_err_invalid;
    const int slot = g_preopen_slot[index];
    const char* path = g_dirs[slot].path;
    // A volume is named without its separator (`C:'), so the rest of a name beneath it does not
    // begin with one; the current directory by its whole name.
    u32 n = length(path);
    if (index > 0 && is_root(path)) n = 2;
    if (name_len) *name_len = n;
    if (name_out && name_cap) {
        const u32 k = n < name_cap ? n : static_cast<u32>(name_cap);
        copy(name_out, path, k);
    }
    out->h = static_cast<kal_uintptr>(slot + 1);
    return kal_ok;
}

kal_uintptr kal_fs_props(kal_dir d) {
    if (!dir_of(d)) return 0;
    return KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_CAPACITY | KAL_FS_PROP_RENAME_NO_REPLACE
         | KAL_FS_PROP_LIST_INFO;
}

kal_uintptr kal_fs_max_name(void) { return kMaxName; }

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len, kal_dir* out) {
    if (!out) return kal_err_invalid;
    char path[kPath];
    if (int e = join(base, name, len, path)) return e;
    const int a = attributes(path);
    if (a < 0) return kal_err_not_found;
    if (!(a & 0x10)) return kal_err_not_directory;
    const int slot = new_dir(path);
    if (slot < 0) return kal_err_no_memory;
    out->h = static_cast<kal_uintptr>(slot + 1);
    return kal_ok;
}

int kal_fs_open(kal_dir base, const char* name, kal_uintptr len, kal_uintptr flags, kal_file* out) {
    if (!out) return kal_err_invalid;
    if (flags & ~(KAL_OPEN_READ | KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_EXCLUSIVE
                  | KAL_OPEN_TRUNCATE | KAL_OPEN_APPEND))
        return kal_err_invalid;
    const bool write = flags & (KAL_OPEN_WRITE | KAL_OPEN_APPEND);
    const bool read = flags & KAL_OPEN_READ;
    if (!read && !write) return kal_err_invalid;
    char path[kPath];
    if (int e = join(base, name, len, path)) return e;
    const int a = attributes(path);
    if (a >= 0 && (a & 0x10)) return kal_err_is_directory;
    rm_regs r{};
    r.edx = 0;
    put_name(path, 0);
    if ((flags & KAL_OPEN_CREATE) && (flags & KAL_OPEN_EXCLUSIVE)) {
        r.eax = 0x5b00; r.ecx = 0;                       // create a new file: refused where one exists
    } else if (a < 0 && (flags & KAL_OPEN_CREATE)) {
        r.eax = 0x5b00; r.ecx = 0;
    } else if (a < 0) {
        return kal_err_not_found;
    } else if ((flags & KAL_OPEN_TRUNCATE) && write) {
        r.eax = 0x3c00; r.ecx = 0;                       // create or truncate
    } else {
        r.eax = 0x3d00u | (read && write ? 2u : write ? 1u : 0u);
    }
    if (!dos(r)) return error_of(r);
    u16 handle = static_cast<u16>(r.eax & 0xffff);
    // 3Ch and 5Bh open for reading and writing whatever was asked; a file meant for reading only
    // is opened again so.
    if ((r.eax & 0xffff) && (flags & KAL_OPEN_CREATE) && !write) {
        rm_regs c{};
        c.eax = 0x3e00; c.ebx = handle;
        dos(c);
        put_name(path, 0);
        rm_regs o{};
        o.eax = 0x3d00; o.edx = 0;
        if (!dos(o)) return error_of(o);
        handle = static_cast<u16>(o.eax & 0xffff);
    }
    if (handle >= 256) {
        rm_regs c{};
        c.eax = 0x3e00; c.ebx = handle;
        dos(c);
        return kal_err_no_memory;
    }
    g_append[handle] = flags & KAL_OPEN_APPEND;
    g_writable[handle] = write;
    out->h = static_cast<kal_uintptr>(handle);
    return kal_ok;
}

void kal_fs_close_dir(kal_dir d) {
    if (directory* p = dir_of(d)) {
        for (int i = 0; i < g_preopens; ++i)
            if (g_preopen_slot[i] == static_cast<int>(d.h - 1)) return;   // supplied: kept
        p->used = false;
        p->list.active = false;
    }
}

void kal_fs_close_file(kal_file f) {
    if (f.h < 5 || f.h >= 256) return;
    rm_regs r{};
    r.eax = 0x3e00; r.ebx = static_cast<u32>(f.h);
    dos(r);
    g_append[f.h] = false;
    g_writable[f.h] = false;
}

kal_stream kal_fs_stream(kal_file f) { return kal_stream{f.h}; }

int kal_fs_seek(kal_file f, kal_i64 offset, int whence, kal_u64* result) {
    if (whence < 0 || whence > 2 || f.h < 5) return kal_err_invalid;
    if (offset > 0x7fffffffll || offset < -0x80000000ll) return kal_err_invalid;
    const u32 o = static_cast<u32>(offset);
    rm_regs r{};
    r.eax = 0x4200u | static_cast<u32>(whence); r.ebx = static_cast<u32>(f.h);
    r.ecx = o >> 16; r.edx = o & 0xffff;
    if (!dos(r)) return error_of(r);
    if (result) *result = ((r.edx & 0xffff) << 16) | (r.eax & 0xffff);
    return kal_ok;
}

int kal_fs_truncate(kal_file f, kal_u64 size) {
    if (size > 0x7fffffffull) return kal_err_invalid;
    kal_u64 here = 0, end = 0;
    if (int e = kal_fs_seek(f, 0, KAL_SEEK_CURRENT, &here)) return e;
    if (int e = kal_fs_seek(f, 0, KAL_SEEK_END, &end)) return e;
    int result = kal_ok;
    if (size > end) {
        // DOS extends a file with whatever its clusters held; openkal extends it with zero.
        u8 zero[512];
        for (auto& b : zero) b = 0;
        for (kal_u64 left = size - end; left;) {
            const kal_uintptr k = left > sizeof zero ? sizeof zero : static_cast<kal_uintptr>(left);
            const kal_intptr w = write_handle(static_cast<u16>(f.h), zero, k);
            if (w <= 0) { result = kal_err_no_space; break; }
            left -= static_cast<kal_u64>(w);
        }
    } else {
        kal_fs_seek(f, static_cast<kal_i64>(size), KAL_SEEK_SET, nullptr);
        rm_regs r{};
        r.eax = 0x4000; r.ebx = static_cast<u32>(f.h); r.ecx = 0; r.edx = 0;   // zero bytes: the end
        if (!dos(r)) result = error_of(r);
    }
    kal_fs_seek(f, static_cast<kal_i64>(here), KAL_SEEK_SET, nullptr);
    return result;
}

kal_intptr kal_fs_read_at(kal_file f, void* buf, kal_uintptr len, kal_u64 offset) {
    kal_u64 here = 0;
    if (int e = kal_fs_seek(f, 0, KAL_SEEK_CURRENT, &here)) return -e;
    if (offset > 0x7fffffffull) return -kal_err_invalid;
    if (int e = kal_fs_seek(f, static_cast<kal_i64>(offset), KAL_SEEK_SET, nullptr)) return -e;
    kal_uintptr done = 0;
    while (done < len) {
        const kal_intptr n = read_handle(static_cast<u16>(f.h), static_cast<u8*>(buf) + done, len - done);
        if (n < 0) { if (!done) done = static_cast<kal_uintptr>(n); break; }
        if (n == 0) break;
        done += static_cast<kal_uintptr>(n);
    }
    kal_fs_seek(f, static_cast<kal_i64>(here), KAL_SEEK_SET, nullptr);
    return static_cast<kal_intptr>(done);
}

kal_intptr kal_fs_write_at(kal_file f, const void* buf, kal_uintptr len, kal_u64 offset) {
    if (f.h >= 256 || !g_writable[f.h]) return -kal_err_permission;
    kal_u64 here = 0;
    if (int e = kal_fs_seek(f, 0, KAL_SEEK_CURRENT, &here)) return -e;
    if (offset > 0x7fffffffull) return -kal_err_invalid;
    if (int e = kal_fs_seek(f, static_cast<kal_i64>(offset), KAL_SEEK_SET, nullptr)) return -e;
    const kal_intptr n = write_handle(static_cast<u16>(f.h), static_cast<const u8*>(buf), len);
    kal_fs_seek(f, static_cast<kal_i64>(here), KAL_SEEK_SET, nullptr);
    return n;
}

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len, kal_uintptr flags, kal_u32 wanted,
                kal_node_info* out) {
    if (!out || out->self_size < 8) return kal_err_invalid;
    if (flags & ~KAL_FS_NO_RESOLVE) return kal_err_invalid;
    char path[kPath];
    if (int e = join(base, name, len, path)) return e;
    if (is_root(path)) {
        fill(out, kal_node_directory, wanted, false, 0, false, 0, -1);
        return kal_ok;
    }
    // Find first with every attribute: the entry's own attributes, size, date and time.
    if (!find_first(path, 0x37)) {
        fill(out, kal_node_absent, wanted, false, 0, false, 0, -1);
        return kal_ok;
    }
    const u8* dta = transfer() + 512;
    const int attr = dta[21];
    const bool dir = attr & 0x10;
    fill(out, dir ? kal_node_directory : kal_node_file, wanted, !dir, dta_u32(dta, 26), true,
         dos_time_ns(dta_u16(dta, 24), dta_u16(dta, 22)), attr);
    return kal_ok;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    char path[kPath];
    if (int e = join(base, name, len, path)) return e;
    if (attributes(path) >= 0) return kal_err_exists;
    put_name(path, 0);
    rm_regs r{};
    r.eax = 0x3900; r.edx = 0;
    return dos(r) ? kal_ok : error_of(r);
}

int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    char path[kPath];
    if (int e = join(base, name, len, path)) return e;
    const int a = attributes(path);
    if (a < 0) return kal_err_not_found;
    put_name(path, 0);
    rm_regs r{};
    r.eax = (a & 0x10) ? 0x3a00 : 0x4100; r.edx = 0;
    if (dos(r)) return kal_ok;
    // A directory that holds entries is refused as access denied, which is what it means here.
    if ((a & 0x10) && (r.eax & 0xffff) == 5) return kal_err_not_empty;
    return error_of(r);
}

int kal_fs_rename_flags(kal_dir from, const char* a, kal_uintptr alen, kal_dir to, const char* b,
                        kal_uintptr blen, kal_uintptr flags) {
    if (flags & ~KAL_RENAME_NO_REPLACE) return kal_err_invalid;
    char pa[kPath], pb[kPath];
    if (int e = join(from, a, alen, pa)) return e;
    if (int e = join(to, b, blen, pb)) return e;
    const int ab = attributes(pb);
    if (ab >= 0) {
        if (flags & KAL_RENAME_NO_REPLACE) return kal_err_exists;
        // The rename that replaces: DOS refuses a name that exists, so it is removed first.
        if (ab & 0x10) return kal_err_is_directory;
        put_name(pb, 0);
        rm_regs del{};
        del.eax = 0x4100; del.edx = 0;
        if (!dos(del)) return error_of(del);
    }
    put_name(pa, 0);
    put_name(pb, 256);
    rm_regs r{};
    r.eax = 0x5600; r.edx = 0; r.edi = 256;
    return dos(r) ? kal_ok : error_of(r);
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen, kal_dir to, const char* b,
                  kal_uintptr blen) {
    return kal_fs_rename_flags(from, a, alen, to, b, blen, 0);
}

int kal_fs_file_info(kal_file f, kal_u32 wanted, kal_node_info* out) {
    if (!out || out->self_size < 8 || f.h < 5) return kal_err_invalid;
    kal_u64 here = 0, end = 0;
    if (int e = kal_fs_seek(f, 0, KAL_SEEK_CURRENT, &here)) return e;
    kal_fs_seek(f, 0, KAL_SEEK_END, &end);
    kal_fs_seek(f, static_cast<kal_i64>(here), KAL_SEEK_SET, nullptr);
    rm_regs t{};
    t.eax = 0x5700; t.ebx = static_cast<u32>(f.h);
    const bool timed = dos(t);
    fill(out, kal_node_file, wanted, true, end, timed, timed ? dos_time_ns(t.edx & 0xffff, t.ecx & 0xffff) : 0, -1);
    return kal_ok;
}

int kal_fs_set_modified(kal_file f, kal_u64 modified_ns) {
    u32 date = 0, time = 0;
    if (!ns_dos_time(modified_ns, date, time)) return kal_err_invalid;
    rm_regs r{};
    r.eax = 0x5701; r.ebx = static_cast<u32>(f.h); r.ecx = time; r.edx = date;
    return dos(r) ? kal_ok : error_of(r);
}

int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len, kal_u64 modified_ns) {
    kal_file f{};
    if (int e = kal_fs_open(base, name, len, KAL_OPEN_READ, &f)) return e;
    const int r = kal_fs_set_modified(f, modified_ns);
    kal_fs_close_file(f);
    return r;
}

int kal_fs_set_executable_at(kal_dir, const char*, kal_uintptr, int) { return kal_err_not_supported; }
int kal_fs_lock(kal_file, kal_u64, kal_u64, kal_uintptr) { return kal_err_not_supported; }
int kal_fs_unlock(kal_file, kal_u64, kal_u64) { return kal_err_not_supported; }

int kal_fs_capacity(kal_dir d, kal_u64* total, kal_u64* available) {
    const directory* p = dir_of(d);
    if (!p) return kal_err_invalid;
    char letter = p->path[0];
    if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
    rm_regs r{};
    r.eax = 0x3600; r.edx = static_cast<u32>(letter - 'A' + 1);
    if (!dos(r) || (r.eax & 0xffff) == 0xffff) return kal_err_not_found;
    const kal_u64 cluster = static_cast<kal_u64>(r.eax & 0xffff) * (r.ecx & 0xffff);
    if (total) *total = cluster * (r.edx & 0xffff);
    if (available) *available = cluster * (r.ebx & 0xffff);
    return kal_ok;
}

int kal_fs_link_create(kal_dir, const char*, kal_uintptr, const char*, kal_uintptr, kal_uintptr) {
    return kal_err_not_supported;
}
kal_intptr kal_fs_link_read(kal_dir, const char*, kal_uintptr, char*, kal_uintptr) {
    return -kal_err_not_supported;
}
int kal_fs_hard_link(kal_dir, const char*, kal_uintptr, kal_dir, const char*, kal_uintptr) {
    return kal_err_not_supported;
}
kal_intptr kal_fs_resolved_name(kal_dir, const char*, kal_uintptr, char*, kal_uintptr) {
    return -kal_err_not_supported;
}
int kal_fs_dir_attenuate(kal_dir, const kal_dir_rights*, kal_dir*) { return kal_err_not_supported; }

int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    directory* p = dir_of(d);
    if (!p || !iter) return kal_err_invalid;
    p->list.active = true;
    p->list.first = true;
    *iter = 1;
    return kal_ok;
}

int kal_fs_list_next_info(kal_dir d, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                          kal_uintptr* name_len, kal_u32 wanted, kal_node_info* info) {
    directory* p = dir_of(d);
    if (!p || !iter) return kal_err_invalid;
    if (*iter == 0 || !p->list.active) {
        *iter = 0;
        if (name_len) *name_len = 0;
        return kal_ok;
    }
    for (;;) {
        bool found;
        if (p->list.first) {
            char pattern[kPath];
            u32 n = length(p->path);
            copy(pattern, p->path, n);
            if (n && pattern[n - 1] != '\\') pattern[n++] = '\\';
            copy(pattern + n, "*.*", 4);
            found = find_first(pattern, 0x37);
            p->list.first = false;
        } else {
            rm_regs set{};
            set.eax = 0x1a00; set.edx = 512;
            dos(set);
            copy(transfer() + 512, p->list.dta, sizeof p->list.dta);
            rm_regs r{};
            r.eax = 0x4f00;
            found = dos(r);
        }
        if (!found) {
            p->list.active = false;
            *iter = 0;
            if (name_len) *name_len = 0;
            return kal_ok;
        }
        copy(p->list.dta, transfer() + 512, sizeof p->list.dta);
        const u8* dta = p->list.dta;
        const char* name = reinterpret_cast<const char*>(dta + 30);
        const u32 n = length(name);
        if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) continue;
        if (dta[21] & 0x08) continue;   // a volume label is not an entry of the directory
        if (name_len) *name_len = n;
        if (name_out && name_cap) copy(name_out, name, n < name_cap ? n : static_cast<u32>(name_cap));
        const bool dir = dta[21] & 0x10;
        if (info)
            fill(info, dir ? kal_node_directory : kal_node_file, wanted, !dir, dta_u32(dta, 26), true,
                 dos_time_ns(dta_u16(dta, 24), dta_u16(dta, 22)), dta[21]);
        return kal_ok;
    }
}

int kal_fs_list_next(kal_dir d, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    kal_node_info info{};
    info.self_size = sizeof info;
    const int r = kal_fs_list_next_info(d, iter, name_out, name_cap, name_len, KAL_INFO_KIND, &info);
    if (r == kal_ok && kind) *kind = *iter ? info.kind : kal_node_absent;
    return r;
}

}  // extern "C"
