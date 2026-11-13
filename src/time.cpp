// openkal.time on DOS.
//
// MONOTONIC: the BIOS's tick count (linear 0x46C), which the timer interrupt advances 1,573,040
// times a day (about 18.2 a second) and sets back to zero at midnight; each reading that is below
// the last one counts a day. Its granularity is one tick, 54,925,494 nanoseconds.
// WALL: DOS's date and time (int 21h, 2Ah and 2Ch), to the hundredth of a second. DOS keeps local
// time and states no zone; it is reported as UTC, which it is where the machine's clock was set so.
// SLEEP: the BIOS's wait (int 15h, 86h), in microseconds.
#include <openkal/time.h>

#include "dos.h"

namespace {

using namespace okdos;

constexpr kal_u64 kTicksPerDay = 0x1800B0;
constexpr kal_u64 kTickNs = 54925494;

}  // namespace

extern "C" {

kal_duration kal_time_monotonic(void) {
    state& s = self();
    const u32 now = *reinterpret_cast<volatile u32*>(low(0x46c));
    if (now < s.ticks_last) ++s.ticks_days;
    s.ticks_last = now;
    const kal_u64 ticks = static_cast<kal_u64>(s.ticks_days) * kTicksPerDay + now;
    return static_cast<kal_duration>(ticks * kTickNs);
}

kal_duration kal_time_monotonic_granularity(void) { return kTickNs; }

kal_duration kal_time_wall(void) {
    rm_regs d{};
    d.eax = 0x2a00;
    if (!dos(d)) return 0;
    rm_regs t{};
    t.eax = 0x2c00;
    if (!dos(t)) return 0;
    const long long year = d.ecx & 0xffff;
    const unsigned month = (d.edx >> 8) & 0xff, day = d.edx & 0xff;
    const unsigned hour = (t.ecx >> 8) & 0xff, minute = t.ecx & 0xff;
    const unsigned second = (t.edx >> 8) & 0xff, hundredths = t.edx & 0xff;
    const long long secs = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    if (secs < 0) return 0;
    return static_cast<kal_duration>(secs) * 1000000000ull + static_cast<kal_duration>(hundredths) * 10000000ull;
}

void kal_time_sleep(kal_duration ns) {
    kal_duration us = (ns + 999) / 1000;
    while (us) {
        const kal_duration step = us > 1000000 ? 1000000 : us;
        rm_regs r{};
        r.eax = 0x8600;
        r.ecx = static_cast<u32>(step >> 16);
        r.edx = static_cast<u32>(step & 0xffff);
        real_int(0x15, r);
        us -= step;
    }
}

kal_uintptr kal_time_props(void) { return KAL_TIME_PROP_WALL_AVAILABLE; }

}  // extern "C"
