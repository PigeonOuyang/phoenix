// SPDX-License-Identifier: Unlicense
#include "ui.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <time.h>

namespace p4ui {
namespace {
std::string line(const char* path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    if (value.size() > 256) return {};
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    return value;
}
}

void refresh_clock(Telemetry& telemetry) {
    // Keep the inherited setting. A setting read on a preceding refresh must
    // not become an inherited setting and mask later host offset updates.
    static const std::string inherited = std::getenv("TZ") ? std::getenv("TZ") : "";
    auto zone = inherited;
    if (zone.empty()) zone = line("/etc/TZ");
    if (zone.empty()) {
        const auto name = line("/etc/timezone");
        if (!name.empty() && name.front() != '/' && name.find("..") == std::string::npos &&
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-") ==
                std::string::npos && std::filesystem::exists("/usr/share/zoneinfo/" + name)) zone = name;
    }
    if (zone.empty() && std::filesystem::exists("/etc/localtime")) {
        std::error_code error;
        const auto path = std::filesystem::canonical("/etc/localtime", error);
        if (!error) zone = ":" + path.string();
    }
    if (zone.empty()) zone = line("/run/overpressure/host-timezone");
    if (zone.empty()) zone = "UTC0";
    setenv("TZ", zone.c_str(), 1);
    tzset();
    format_clock(telemetry, time(nullptr));
}

void format_clock(Telemetry& telemetry, std::int64_t epoch) {
    telemetry.epoch=epoch;
    const time_t now = epoch;
    // Display an initialized system clock without claiming NTP verification.
    // Qualcomm's raw RTC can be a 1970-era counter; never display it as today.
    telemetry.time_valid = now >= 1577836800 && now < 4102444800;
    struct tm local{};
    if (!telemetry.time_valid || !localtime_r(&now, &local)) {
        telemetry.time_valid = false;
        telemetry.clock = "--:--:--";
        telemetry.date.clear();
        telemetry.timezone.clear();
        return;
    }
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    telemetry.clock = buffer;
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    telemetry.date = buffer;
    strftime(buffer, sizeof(buffer), "UTC%z", &local);
    telemetry.timezone = buffer;
}

bool display_clock(int zone,std::int64_t epoch,std::tm& out) {
    // Musl resolves these fixed names using packaged tzdata, including historical
    // transitions. This isolated formatting path runs on the compositor thread.
    static constexpr const char* zones[]{"","UTC0","Asia/Shanghai","Asia/Tokyo","Europe/Berlin","America/New_York"};
    if (zone<=0||zone>=6) return false;
    const auto* current=std::getenv("TZ");
    const std::string prior=current?current:"";
    const bool had_zone=current!=nullptr;
    if (setenv("TZ",zones[zone],1)!=0) return false;
    tzset();
    const time_t value=epoch;
    const bool ok=localtime_r(&value,&out)!=nullptr;
    if (had_zone) setenv("TZ",prior.c_str(),1);else unsetenv("TZ");
    tzset();
    return ok;
}
}
