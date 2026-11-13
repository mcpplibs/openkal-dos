// openkal.exec on DOS through DPMI: memory a program may execute, which a loader of images needs
// (the ok-image runner, carried as a DOS program, places an image's code here).
//
// The stub's code descriptor has the block's base and a limit of 4 GiB, so every address of the
// program, a DPMI block allocated since among them, is executable through it: a region is a DPMI
// block, zeroed, at the page granularity. DPMI 0.9 has no protection a client may change on its
// own pages, so publishing and sealing a part make no request; they are statements the caller
// makes, as the interface allows an environment without such a protection.
//
// A file is not mapped (DOS has no mapping): kal_exec_map_part answers kal_err_not_supported, and
// the properties claim neither a file's segments nor a placement.
#include <openkal/exec.h>
#include <openkal/memory.h>
#include <openkal/types.h>

namespace {

constexpr kal_uintptr kPage = 4096;

kal_uintptr pages(kal_uintptr size) { return (size + kPage - 1) / kPage * kPage; }

bool part(void* region, kal_uintptr offset, kal_uintptr size) {
    return region != nullptr && size != 0 && offset % kPage == 0 && size % kPage == 0;
}

}  // namespace

extern "C" {

void* kal_exec_alloc(kal_uintptr size) {
    if (size == 0) return nullptr;
    const kal_uintptr n = pages(size);
    if (n < size) return nullptr;
    auto* p = static_cast<unsigned char*>(kal_alloc(n, kPage));
    if (p)
        for (kal_uintptr i = 0; i < n; ++i) p[i] = 0;
    return p;
}

// Placement at a chosen address is not offered: DPMI 0.9 gives a block where the host places it.
void* kal_exec_alloc_at(void*, kal_uintptr) { return nullptr; }

int kal_exec_publish(void* p, kal_uintptr size) {
    return p != nullptr && size != 0 ? kal_ok : kal_err_invalid;
}

int kal_exec_publish_part(void* region, kal_uintptr offset, kal_uintptr size) {
    return part(region, offset, size) ? kal_ok : kal_err_invalid;
}

int kal_exec_seal_part(void* region, kal_uintptr offset, kal_uintptr size, kal_uintptr flags) {
    if (flags & ~KAL_EXEC_SEAL_FOREVER) return kal_err_invalid;
    return part(region, offset, size) ? kal_ok : kal_err_invalid;
}

kal_uintptr kal_exec_granularity(void) { return kPage; }

int kal_exec_map_part(void*, kal_uintptr, kal_uintptr, struct kal_file, kal_u64, kal_uintptr) {
    return kal_err_not_supported;
}

void kal_exec_free(void* p, kal_uintptr size) {
    if (p) kal_free(p, pages(size), kPage);
}

kal_uintptr kal_exec_props(void) { return KAL_EXEC_PROP_AVAILABLE; }

}  // extern "C"
