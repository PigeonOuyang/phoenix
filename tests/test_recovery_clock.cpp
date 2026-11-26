// SPDX-License-Identifier: Unlicense
#include "../src/recovery-ui/ui.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <time.h>
using namespace p4ui;
void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
void zone(const char* value) { setenv("TZ", value, 1); tzset(); }
int main() {
    Telemetry clock;
    check(clock.time_source == "system", "new telemetry defaults to the system clock source ID");
    zone("UTC0");
    format_clock(clock, 1795543200); // 2026-11-24 18:00 UTC
    check(clock.time_valid && clock.clock == "18:00:00", "initialized system clock needs no marker");
    check(clock.date == "2026-11-24", "UTC date contains only the numeric date");
    zone("UTC-08:00");
    format_clock(clock, 1795543200);
    check(clock.clock == "02:00:00" && clock.date == "2026-11-25", "host offset crosses date boundary");
    check(clock.timezone == "UTC+0800", "POSIX offset sign");
    zone("UTC+05:30");
    format_clock(clock, 1795543200);
    check(clock.clock == "12:30:00" && clock.timezone == "UTC-0530", "negative fractional offset");
    zone("EST5EDT,M3.2.0,M11.1.0");
    format_clock(clock, 1772953199); // Before 2026 spring DST transition
    check(clock.clock == "01:59:59" && clock.timezone == "UTC-0500", "system timezone before DST");
    format_clock(clock, 1772953200);
    check(clock.clock == "03:00:00" && clock.timezone == "UTC-0400", "system timezone after DST");
    for (auto value : {std::int64_t{-1}, std::int64_t{6496896}, std::int64_t{1577836799}, std::int64_t{4102444800}}) {
        format_clock(clock, value);
        check(!clock.time_valid && clock.clock == "--:--:--" && clock.date.empty() && clock.timezone.empty(),
              "uninitialized or unsupported clock does not retain stale display");
    }
    format_clock(clock, 1577836800);
    check(clock.time_valid, "initialized clock lower boundary");
    std::cout << "RECOVERY_CLOCK_OK 13 checks\n";
}
