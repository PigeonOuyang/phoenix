// SPDX-License-Identifier: Unlicense
#include "preferences.cpp"

#include <iostream>

namespace {
void check(bool okay, const char* description) {
    if (!okay) throw std::runtime_error(description);
}

void test_clock_data() {
    p4ui::Telemetry clock;
    check(setenv("TZ", "UTC0", 1) == 0, "set UTC");
    tzset();
    constexpr std::int64_t sample = 1795543200;
    p4ui::format_clock(clock, sample);
    check(clock.time_valid && clock.clock == "18:00:00" && clock.date == "2026-11-24" &&
          clock.timezone == "UTC+0000", "clock emits numeric fields without display text");
    check(setenv("TZ", "UTC-8", 1) == 0, "set host offset");
    tzset();
    p4ui::format_clock(clock, sample);
    check(clock.time_valid && clock.clock == "02:00:00" && clock.date == "2026-11-25",
          "date follows the configured timezone");
    p4ui::format_clock(clock, 0);
    check(!clock.time_valid && clock.clock == "--:--:--" && clock.date.empty() &&
          clock.timezone.empty(), "invalid clock clears old date and timezone");
}

void test_preferences_diagnostics() {
    using namespace p4ui;
    Preferences reply;
    check(!save_preferences_native("unsupported", reply) && !reply.durable &&
          reply.error == "Unsupported language; saved locale was not changed",
          "native rejection emits a plain English diagnostic");
    check(!save_preferences("unsupported", reply) && !reply.durable &&
          reply.error == "Unsupported language; saved locale was not changed",
          "request rejection emits the same English diagnostic");

    const auto object = parse(R"({"version":1,"locale":"unsupported","language_selected":true})");
    check(decoded(object.get(), "cache").error == "Unsupported saved locale; choose a supported language again",
          "stored unsupported locale has its own diagnostic");
    const auto foreign = installer_reply(
        R"({"version":1,"locale":"en","language_selected":false,"storage":"none","durable":false,"error":"\u539f\u59cb\u8bca\u65ad"})",
        1 << 8, "en");
    check(!foreign.durable && foreign.error == "\u539f\u59cb\u8bca\u65ad",
          "installer error text passes through verbatim");
}
}

int main() {
    try {
        test_clock_data();
        test_preferences_diagnostics();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Recovery producer language tests passed\n";
}
