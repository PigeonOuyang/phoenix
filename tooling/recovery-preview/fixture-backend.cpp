// SPDX-License-Identifier: Unlicense
#include "fixture-backend.hpp"

#include <chrono>
#include <mutex>

namespace p4ui {
namespace {

std::mutex preferences_lock;
Preferences fixture_preferences;
std::vector<std::string> fixture_logs;

} // namespace

namespace preview {

void set_fixture_logs(const std::vector<std::string>& log) { fixture_logs = log; }

void set_fixture_locale(std::string_view locale) {
    std::lock_guard lock(preferences_lock);
    fixture_preferences.locale = locale;
    fixture_preferences.language_selected = fixture_preferences.durable = true;
    fixture_preferences.storage = "preview";
}

void set_fixture_telemetry(Telemetry& telemetry) {
    telemetry.capacity = 82;
    telemetry.net_watts = 4.92;
    telemetry.battery_status = "Charging";
    telemetry.ncm_state = "connected";
    telemetry.time_valid = true;
    telemetry.clock = "02:56:22";
    telemetry.date = "2026-11-26";
}

} // namespace preview

std::string Backend::read(const std::string&, std::size_t) const { return {}; }

std::vector<std::string> Backend::logs(const std::string& source) const {
    if (source == "task" || source.ends_with("/task.log")) return fixture_logs;
    if (source == "boot" || source.ends_with("/boot.log"))
        return {"Recovery preview / synthetic telemetry"};
    return {};
}

void Backend::refresh(bool) {}
bool Backend::has_system_image() const { return false; }
Preferences Backend::load_ui_preferences() const {
    std::lock_guard lock(preferences_lock);
    return fixture_preferences;
}

bool Backend::save_ui_preferences(const Preferences& wanted, bool, Preferences& result) const {
    std::lock_guard lock(preferences_lock);
    fixture_preferences = wanted;
    fixture_preferences.language_selected = fixture_preferences.durable = true;
    fixture_preferences.storage = "preview";
    fixture_preferences.error.clear();
    result = fixture_preferences;
    return true;
}

bool Backend::start(const std::string&, const std::string&, std::string& error) {
    error = "Local preview cannot dispatch Recovery actions.";
    return false;
}

std::vector<std::string> supported_locales() { return {"en", "zh-CN"}; }

double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace p4ui
