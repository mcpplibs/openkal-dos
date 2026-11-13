// openkal's core interfaces on DOS, through DPMI (openkal-dos; technical experiment only).
//
// DOS divides its services as openkal does for three interfaces: handles 0 to 2 are the standard
// streams (int 21h, 3Fh and 40h), the program ends with a status (4Ch), and memory beyond the
// first megabyte is the DPMI host's (int 31h, 0501h). The interfaces DOS has no counterpart for
// --- contexts, processes as openkal states them, the network --- are absent rather than present
// and failing (openkal clause 6.2).
#include <openkal/abort.h>
#include <openkal/memory.h>
#include <openkal/stream.h>
#include <openkal/version.h>

#include "dos.h"

namespace {

using namespace okdos;

constexpr kal_uintptr kStdin = 0, kStdout = 1, kStderr = 2;

// Writes through the transfer buffer, a piece at a time.
kal_intptr write_handle(u16 handle, const u8* p, kal_uintptr n) {
    kal_uintptr done = 0;
    while (done < n) {
        u32 k = static_cast<u32>(n - done);
        if (k > transfer_size()) k = transfer_size();
        copy(transfer(), p + done, k);
        rm_regs r{};
        r.eax = 0x4000; r.ebx = handle; r.ecx = k; r.edx = 0;
        if (!dos(r)) return done ? static_cast<kal_intptr>(done) : -kal_err_io;
        const u32 wrote = r.eax & 0xffff;
        done += wrote;
        if (wrote < k) break;   // a full disk, or a device that takes no more
    }
    return static_cast<kal_intptr>(done);
}

kal_intptr read_handle(u16 handle, u8* p, kal_uintptr n) {
    u32 k = static_cast<u32>(n);
    if (k > transfer_size()) k = transfer_size();
    rm_regs r{};
    r.eax = 0x3f00; r.ebx = handle; r.ecx = k; r.edx = 0;
    if (!dos(r)) return -kal_err_io;
    const u32 got = r.eax & 0xffff;
    copy(p, transfer(), got);
    return static_cast<kal_intptr>(got);
}

// IOCTL 4400h: bit 7 of the device word is "a device, not a file"; bit 0 or 1 the console.
bool is_console(u16 handle) {
    rm_regs r{};
    r.eax = 0x4400; r.ebx = handle;
    return dos(r) && (r.edx & 0x80) && (r.edx & 0x03);
}

}  // namespace

extern "C" {

KAL_NORETURN void kal_exit(int code) {
    __asm__ volatile("int $0x21" ::"a"(0x4c00 | (code & 0xff)));
    for (;;) {}
}

KAL_NORETURN void kal_abort(const char* msg, kal_uintptr len) {
    if (self().h) {
        if (msg && len) write_handle(2, reinterpret_cast<const u8*>(msg), len);
        write_handle(2, reinterpret_cast<const u8*>("\r\n"), 2);
    }
    // 134, as a process ended by SIGABRT reports itself on the systems that have the signal.
    kal_exit(134);
}

// ── openkal.stream ──────────────────────────────────────────────────────────
kal_stream kal_stdin(void) { return kal_stream{kStdin}; }
kal_stream kal_stdout(void) { return kal_stream{kStdout}; }
kal_stream kal_stderr(void) { return kal_stream{kStderr}; }

kal_intptr kal_stream_write(kal_stream s, const void* buf, kal_uintptr n) {
    if (s.h != kStdout && s.h != kStderr) return -kal_err_invalid;
    return write_handle(static_cast<u16>(s.h), static_cast<const u8*>(buf), n);
}

kal_intptr kal_stream_read(kal_stream s, void* buf, kal_uintptr n) {
    if (s.h != kStdin) return -kal_err_invalid;
    return read_handle(0, static_cast<u8*>(buf), n);
}

// DOS writes a handle's bytes when the call returns.
int kal_stream_flush(kal_stream s) {
    return s.h <= kStderr ? kal_ok : kal_err_invalid;
}

kal_uintptr kal_stream_props(kal_stream s) {
    if (s.h > kStderr) return 0;
    return is_console(static_cast<u16>(s.h)) ? KAL_STREAM_PROP_INTERACTIVE : 0;
}

// ── openkal.memory ──────────────────────────────────────────────────────────
//
// Each region is a block of the DPMI host (0501h), large enough to place the region at its
// alignment with a record of the block before it: its handle and its linear address. The host
// allocates in pages of four kilobytes, which is the granularity stated.
kal_uintptr kal_memory_granularity(void) { return 4096; }

void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (!self().h) return nullptr;
    if (size == 0) size = 1;
    if (align < 16) align = 16;
    if ((align & (align - 1)) != 0) return nullptr;
    const u32 want = static_cast<u32>(size + align + 16);
    if (want < size) return nullptr;
    u32 bx = want >> 16, cx = want & 0xffff, si, di, carry;
    __asm__ volatile("int $0x31\n\tsbb %4, %4"
                     : "+b"(bx), "+c"(cx), "=S"(si), "=D"(di), "=r"(carry)
                     : "a"(0x0501)
                     : "memory", "cc");
    if (carry) return nullptr;
    const u32 linear = (bx << 16) | (cx & 0xffff);
    const u32 handle = (si << 16) | (di & 0xffff);
    const u32 base = self().h->base;
    u32 at = (linear - base + 16 + static_cast<u32>(align) - 1) & ~(static_cast<u32>(align) - 1);
    auto* record = reinterpret_cast<u32*>(static_cast<unsigned long>(at - 8));
    record[0] = handle;
    record[1] = linear;
    return reinterpret_cast<void*>(static_cast<unsigned long>(at));
}

void kal_free(void* p, kal_uintptr, kal_uintptr) {
    if (!p) return;
    const auto* record = reinterpret_cast<const u32*>(static_cast<u8*>(p) - 8);
    const u32 handle = record[0];
    u32 si = handle >> 16, di = handle & 0xffff;
    __asm__ volatile("int $0x31" : "+S"(si), "+D"(di) : "a"(0x0502) : "memory", "cc");
}

// DPMI 0500h: the largest free block, and, where the host states them, the pages in total and free.
int kal_memory_info(kal_memory_status* out) {
    if (out == nullptr || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    u32 info[12];
    for (auto& w : info) w = ~0u;
    int carry;
    __asm__ volatile("push %%es\n\tpush %%ds\n\tpop %%es\n\tint $0x31\n\tpop %%es\n\tsbb %0, %0"
                     : "=r"(carry) : "a"(0x0500), "D"(info) : "memory", "cc");
    out->present = 0;
    if (carry) return kal_ok;
    if (out->self_size >= sizeof(kal_memory_status)) {
        if (info[6] != ~0u) { out->total_bytes = static_cast<kal_u64>(info[6]) * 4096; out->present |= KAL_MSTATUS_TOTAL; }
        if (info[0] != ~0u) { out->available_bytes = info[0]; out->present |= KAL_MSTATUS_AVAILABLE; }
    }
    return kal_ok;
}

// ── version ─────────────────────────────────────────────────────────────────
kal_u64 kal_version(void) { return KAL_VERSION; }

kal_u64 kal_interfaces(void) {
    return KAL_IFACE_ABORT | KAL_IFACE_STREAM | KAL_IFACE_MEMORY | KAL_IFACE_ENV | KAL_IFACE_TIME;
}

}  // extern "C"
