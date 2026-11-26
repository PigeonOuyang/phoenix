// SPDX-License-Identifier: Unlicense
#include "ui.hpp"
#include "locale.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <json-c/json.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace p4ui {
namespace {
constexpr auto run = "/run/overpressure/";
constexpr auto preview_dir = "/run/overpressure/ui-install-preview/";
constexpr auto helper = "/usr/libexec/pico4/task-state";
std::map<Backend*, pid_t> children;
struct FilesystemCache { std::string type; double checked = 0; std::uint64_t bytes = 0; };
std::map<std::string, FilesystemCache> filesystems;

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

template<class T> std::optional<T> number(const std::string& text) {
    const auto value = trim(text);
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
    return result;
}

bool partition_name(const std::string& text) {
    return !text.empty() && text != "." && text != ".." && text.size() <= 128 &&
        text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
            std::string::npos;
}

std::string canonical(const std::string& path) {
    std::error_code ec;
    const auto result = std::filesystem::canonical(path, ec);
    return ec ? std::string{} : result.string();
}

bool block_device(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
}

std::uint64_t device_bytes(const Backend& backend, const std::string& device) {
    const auto node = std::filesystem::path(device).filename().string();
    const auto sectors = number<std::uint64_t>(backend.read("/sys/class/block/" + node + "/size", 64));
    if (sectors && *sectors <= std::numeric_limits<std::uint64_t>::max() / 512) return *sectors * 512;
    const int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    std::uint64_t bytes = 0;
    if (fd >= 0) {
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) bytes = 0;
        close(fd);
    }
    return bytes;
}

std::uint32_t le32(const unsigned char* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::string filesystem(const std::string& device) {
    std::array<unsigned char, 4096> bytes{};
    const int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    const auto count = pread(fd, bytes.data(), bytes.size(), 0);
    close(fd);
    if (count < 2048) return {};
    if (bytes[1080] == 0x53 && bytes[1081] == 0xef) {
        if (le32(bytes.data() + 1120) & (0x40 | 0x80 | 0x200)) return "ext4";
        return (le32(bytes.data() + 1116) & 4) ? "ext3" : "ext2";
    }
    if (le32(bytes.data() + 1024) == 0xf2f52010) return "f2fs";
    if (le32(bytes.data() + 1024) == 0xe0f5e1e2) return "erofs";
    if (le32(bytes.data()) == 0x73717368) return "squashfs";
    if (bytes[510] == 0x55 && bytes[511] == 0xaa &&
        (std::memcmp(bytes.data() + 54, "FAT", 3) == 0 || std::memcmp(bytes.data() + 82, "FAT32", 5) == 0))
        return "vfat";
    return {};
}

bool mountable(const std::string& type) {
    return type == "ext2" || type == "ext3" || type == "ext4" || type == "f2fs" ||
        type == "vfat" || type == "squashfs" || type == "erofs";
}

std::string mount_field(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() &&
            text[i+1] >= '0' && text[i+1] <= '7' && text[i+2] >= '0' && text[i+2] <= '7' &&
            text[i+3] >= '0' && text[i+3] <= '7') {
            result += char((text[i+1]-'0')*64 + (text[i+2]-'0')*8 + text[i+3]-'0');
            i += 3;
        } else result += text[i];
    }
    return result;
}

json_object* field(json_object* object, const char* key) {
    json_object* result = nullptr;
    if (object && json_object_is_type(object, json_type_object)) json_object_object_get_ex(object, key, &result);
    return result;
}

std::string text(json_object* object, const char* key) {
    auto* value = field(object, key);
    return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
}

bool flag(json_object* object, const char* key) {
    auto* value = field(object, key);
    return value && json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}

json_object* parse(const std::string& input) {
    if (input.empty()) return nullptr;
    json_tokener* tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object* value = json_tokener_parse_ex(tokener, input.data(), static_cast<int>(input.size()));
    const bool okay = json_tokener_get_error(tokener) == json_tokener_success;
    const auto end = json_tokener_get_parse_end(tokener);
    json_tokener_free(tokener);
    if (!okay || !value || !json_object_is_type(value, json_type_object) || !trim(input.substr(end)).empty()) {
        if (value) json_object_put(value);
        return nullptr;
    }
    return value;
}

bool known_state(const std::string& state) {
    return state == "pending" || state == "running" || state == "waiting" || state == "done" ||
        state == "skipped" || state == "error";
}

bool local_count(const std::string& source, const std::string& unit) {
    if (unit == "bytes")
        return source == "local-written-bytes" || source == "local-readback-bytes" ||
            source == "local-received-bytes" || source == "local-download-bytes" ||
            source == "local-processed-bytes";
    if (unit == "packages") return source == "local-package-count";
    if (unit == "files") return source == "local-verified-files";
    return unit == "percent" && source == "local-check-progress";
}

std::vector<Substep> read_substeps(json_object* stage) {
    auto* substeps = field(stage, "substeps");
    if (!substeps || !json_object_is_type(substeps, json_type_array)) return {};
    std::vector<Substep> plan;
    const auto count = json_object_array_length(substeps);
    if (count > 32) return {};
    for (std::size_t i = 0; i < count; ++i) {
        auto* item = json_object_array_get_idx(substeps, i);
        Substep step;
        step.id = text(item, "id");
        step.name = text(item, "name");
        step.state = text(item, "status");
        step.measured = flag(item, "measured");
        auto* weight = field(item, "weight");
        if (!partition_name(step.id) || !known_state(step.state) || !weight ||
            !(json_object_is_type(weight, json_type_int) || json_object_is_type(weight, json_type_double)) ||
            std::any_of(plan.begin(), plan.end(), [&](const Substep& s){ return s.id == step.id; })) return {};
        step.weight = json_object_get_double(weight);
        if (!(step.weight >= 0 && step.weight <= 86400)) return {};
        const auto unit = text(item, "unit");
        auto* done = field(item, "done");
        auto* total = field(item, "total");
        if (step.measured && step.state != "pending" && flag(item, "progress_trusted") &&
            local_count(text(item, "progress_source"), unit) && done && total &&
            json_object_is_type(done, json_type_int) && json_object_is_type(total, json_type_int)) {
            const auto d = json_object_get_int64(done), n = json_object_get_int64(total);
            if (d >= 0 && n > 0 && d <= n) {
                step.done = d;
                step.total = n;
                step.unit = unit;
                // An operation can consume all bytes before it has finished.
                const auto ratio = static_cast<double>(d) / static_cast<double>(n);
                step.fraction = step.state == "done" ? ratio : std::min(ratio, .999);
            }
        }
        plan.push_back(std::move(step));
    }
    return plan;
}

bool lock_busy() {
    const int fd = open("/run/overpressure/install.lock", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const bool busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return busy;
}

bool log_path(const std::string& source) {
    if (source == "/run/overpressure/boot.log" || source == "/run/overpressure/task.log" ||
        source == "/run/overpressure/install/dd.log") return true;
    const std::filesystem::path path(source);
    const auto name = path.filename().string();
    if (path.parent_path() == "/run/overpressure/report")
        return partition_name(name) && (name.ends_with(".txt") || name.ends_with(".dts"));
    if (path.parent_path() == "/sys/fs/pstore")
        return partition_name(name) && (name.starts_with("console-ramoops") || name.starts_with("pmsg-ramoops"));
    return false;
}

bool publish(const Task& task) {
    auto* object = json_object_new_object();
    if (!object) return false;
    auto string = [&](json_object* to, const char* key, const std::string& value) {
        json_object_object_add(to, key, json_object_new_string(value.c_str()));
    };
    string(object, "id", task.id); string(object, "name", task.name);
    string(object, "status", task.status); string(object, "detail", task.detail); string(object, "error", task.error);
    json_object_object_add(object, "active", json_object_new_int(task.active));
    json_object_object_add(object, "locked", json_object_new_boolean(task.locked));
    json_object_object_add(object, "readonly_restored", json_object_new_boolean(task.readonly_restored));
    auto* stages = json_object_new_array();
    for (const auto& stage : task.stages) {
        auto* item = json_object_new_object();
        string(item, "name", stage.name); string(item, "status", stage.state);
        json_object_array_add(stages, item);
    }
    json_object_object_add(object, "stages", stages);
    std::string bytes = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    json_object_put(object);
    bytes += '\n';
    char path[] = "/run/overpressure/.task.json.XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return false;
    std::size_t used = 0;
    while (used < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + used, bytes.size() - used);
        if (written <= 0) break;
        used += static_cast<std::size_t>(written);
    }
    const bool closed = close(fd) == 0;
    const bool okay = used == bytes.size() && closed && rename(path, "/run/overpressure/task.json") == 0;
    if (!okay) unlink(path);
    return okay;
}
}

double monotonic_seconds() {
    struct timespec value{};
    return clock_gettime(CLOCK_MONOTONIC, &value) == 0 ? value.tv_sec + value.tv_nsec / 1e9 : 0;
}

bool decode_task(const std::string& input, Task& task) {
    auto* object = input.size() <= 65536 ? parse(input) : nullptr;
    if (!object) return false;
    Task decoded;
    decoded.id = text(object, "id");
    decoded.name = text(object, "name");
    decoded.kind = text(object, "kind");
    const auto state = text(object, "status");
    decoded.status = known_state(state) || state == "idle" ? state : "error";
    decoded.detail = text(object, "detail");
    decoded.error = text(object, "error");
    decoded.readonly_restored = flag(object, "readonly_restored");
    decoded.locked = flag(object, "locked");
    auto* stages = field(object, "stages");
    if (stages && json_object_is_type(stages, json_type_array)) {
        const auto count = std::min<std::size_t>(json_object_array_length(stages), 32);
        for (std::size_t i = 0; i < count; ++i) {
            auto* item = json_object_array_get_idx(stages, i);
            Stage stage;
            stage.id = text(item, "id");
            if (!partition_name(stage.id)) stage.id.clear();
            stage.name = text(item, "name");
            stage.state = text(item, "status");
            if (stage.state.empty()) stage.state = text(item, "state");
            if (!known_state(stage.state)) stage.state = "pending";
            if (!stage.id.empty()) stage.substeps = read_substeps(item);
            if (!stage.substeps.empty()) {
                stage.fraction = weighted_progress(stage);
            } else {
                // Older firmware has only a locally observed byte count for writing.
                // Host transmission percentages are not written bytes.
                auto* observed = field(item, "observed_bytes");
                auto* size = field(item, "total_bytes");
                if (text(item, "progress_source") == "local-written-bytes" && flag(item, "progress_trusted") &&
                    observed && size && json_object_is_type(observed, json_type_int) &&
                    json_object_is_type(size, json_type_int)) {
                    const auto done = json_object_get_int64(observed), bytes = json_object_get_int64(size);
                    if (done >= 0 && bytes > 0 && done < bytes && stage.state == "running")
                        stage.fraction = static_cast<double>(done) / static_cast<double>(bytes);
                }
            }
            decoded.stages.push_back(std::move(stage));
        }
    }
    auto* active = field(object, "active");
    if (active && json_object_is_type(active, json_type_int)) {
        const auto index = json_object_get_int64(active);
        if (index >= 0 && index < static_cast<std::int64_t>(decoded.stages.size()))
            decoded.active = static_cast<int>(index);
    }
    json_object_put(object);
    task = std::move(decoded);
    return true;
}

std::string Backend::read(const std::string& path, std::size_t limit) const {
    limit = std::min<std::size_t>(limit, 1024 * 1024);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return {};
    std::string result(limit, '\0');
    std::size_t count = 0;
    while (count < limit) {
        const auto got = ::read(fd, result.data() + count, limit - count);
        if (got <= 0) break;
        count += static_cast<std::size_t>(got);
    }
    close(fd);
    result.resize(count);
    return result;
}

std::vector<std::string> Backend::logs(const std::string& source) const {
    if (install_preview && (source == "/run/overpressure/boot.log" || source == "/run/overpressure/task.log")) {
        const auto path = std::string(preview_dir) + (source.ends_with("boot.log") ? "boot.log" :
            "task." + std::to_string(preview_step) + ".log");
        std::istringstream input(read(path, 256 * 1024));
        std::vector<std::string> lines;
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
        return lines;
    }
    if (source == "boot") return logs("/run/overpressure/boot.log");
    if (source == "kernel") return logs("/run/overpressure/report/dmesg.txt");
    if (source == "services") return logs("/run/overpressure/boot.log");
    if (source == "task") return logs("/run/overpressure/task.log");
    if (source == "pstore") {
        std::vector<std::string> result;
        std::error_code ec;
        std::vector<std::string> paths;
        for (std::filesystem::directory_iterator it("/sys/fs/pstore", ec), end;
             !ec && it != end; it.increment(ec)) {
            if (log_path(it->path().string())) paths.push_back(it->path().string());
            if (paths.size() >= 16) break;
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            auto entries = logs(path);
            result.insert(result.end(), entries.begin(), entries.end());
            if (result.size() > 2048) result.erase(result.begin(), result.end() - 2048);
        }
        return result;
    }
    if (!log_path(source)) return {};
    const int fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return {};
    struct stat st{};
    constexpr std::size_t limit = 256 * 1024;
    bool partial = false;
    if (fstat(fd, &st) == 0 && st.st_size > static_cast<off_t>(limit))
        partial = lseek(fd, -static_cast<off_t>(limit), SEEK_END) >= 0;
    std::string content(limit, '\0');
    std::size_t used = 0;
    while (used < limit) {
        const auto got = ::read(fd, content.data() + used, limit - used);
        if (got <= 0) break;
        used += static_cast<std::size_t>(got);
    }
    close(fd);
    content.resize(used);
    std::vector<std::string> result;
    std::istringstream lines(content);
    std::string line;
    if (partial) std::getline(lines, line);
    while (std::getline(lines, line)) result.push_back(line);
    if (result.size() > 2048) result.erase(result.begin(), result.end() - 2048);
    return result;
}

bool Backend::has_system_image() const {
    if(install_preview)return false;
    // Match pico4-install status's on-device image marker. A label does not
    // establish verified installation or a successful SteamOS boot.
    const auto device=canonical("/dev/block/by-name/super");
    if(!block_device(device))return false;
    const int fd=open(device.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return false;
    std::array<char,80> header{};
    const auto got=pread(fd,header.data(),header.size(),1080);
    close(fd);
    return got==static_cast<ssize_t>(header.size())&&header[0]==0x53&&
        static_cast<unsigned char>(header[1])==0xef&&
        std::string_view(header.data()+64,16)==std::string_view("pico4-system\0\0\0\0",16);
}

Preferences Backend::load_ui_preferences() const {
    if(!install_preview)return load_preferences();
    Preferences result;
    auto encoded=trim(read(std::string(preview_dir)+"language",512));
    if(decode_settings(encoded,result)){
        result.language_selected=result.durable=true;
        result.storage="preview";
    }
    return result;
}

bool Backend::save_ui_preferences(const Preferences& wanted,bool initial,Preferences& result) const {
    if(!install_preview)return initial?save_preferences(wanted.locale,result):save_settings(wanted,result);
    const auto path=std::string(preview_dir)+"language";
    auto temporary=path+".tmp.XXXXXX";
    const int fd=mkstemp(temporary.data());
    if(fd>=0){
        const auto bytes=encode_settings(wanted)+"\n";
        const bool written=write(fd,bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size());
        const bool closed=close(fd)==0;
        if(written&&closed&&rename(temporary.c_str(),path.c_str())==0){
            result=load_ui_preferences();
            if(result.durable&&same_settings(wanted,result))return true;
        }
        unlink(temporary.c_str());
    }
    result.error=msg::no_valid_language_save_result_was_received;
    return false;
}

void Backend::refresh(bool include_partitions) {
    // Timestamp at entry: variable collector/refresh work must not move the 1 Hz gate.
    const double network_seconds = monotonic_seconds();
    telemetry = Telemetry{};
    const auto network = collect_network();
    telemetry.ncm_state = ncm_state_string(classify_ncm(network));
    telemetry.ncm_interface = network.interface_name.value_or("");
    const auto rates = network_rates.sample(network, network_seconds);
    telemetry.upload_bps = rates.upload_bps;
    telemetry.download_bps = rates.download_bps;
    if (const auto& sample = network_rates.last_sample()) {
        telemetry.network_rx_bytes = sample->rx_bytes;
        telemetry.network_tx_bytes = sample->tx_bytes;
        telemetry.network_sample_seconds = sample->monotonic;
    }
    const std::string battery = "/sys/class/power_supply/battery/";
    const auto capacity = number<int>(read(battery + "capacity", 64));
    if (capacity && *capacity >= 0 && *capacity <= 100) telemetry.capacity = capacity;
    const auto status = trim(read(battery + "status", 64));
    if (status == "Charging" || status == "Discharging" || status == "Full" ||
        status == "Not charging" || status == "Unknown") telemetry.battery_status = status;
    const auto current = number<std::int64_t>(read(battery + "current_now", 64));
    const auto voltage = number<std::int64_t>(read(battery + "voltage_now", 64));
    // power_supply current_now alone does not declare its polarity. Require an explicit
    // board adapter marker; status is never a substitute for the actual current direction.
    const auto polarity = trim(read("/run/overpressure/battery-current-direction", 64));
    if (current && voltage && *voltage > 0 &&
        (polarity == "positive-is-charging" || polarity == "positive-is-discharging")) {
        const double watts = static_cast<double>(*current) * static_cast<double>(*voltage) / 1e12;
        if (std::isfinite(watts)) telemetry.net_watts = polarity == "positive-is-charging" ? watts : -watts;
    }

    refresh_clock(telemetry);
    if (auto* receipt = parse(read(std::string(run) + "time-source.json", 4096))) {
        const auto boot = trim(read("/proc/sys/kernel/random/boot_id", 64));
        if (!boot.empty() && text(receipt, "source") == "host" && text(receipt, "boot_id") == boot)
            telemetry.time_source = "host";
        json_object_put(receipt);
    }
    struct utsname system{};
    if (uname(&system) == 0) telemetry.kernel = system.release;
    const auto uptime_text = read("/proc/uptime", 64);
    if (const auto seconds = number<double>(uptime_text.substr(0, uptime_text.find(' ')));
        seconds && *seconds >= 0 && *seconds / 60 < 0x1p64) {
        telemetry.uptime = std::to_string(static_cast<std::uint64_t>(*seconds / 60));
    }
    std::istringstream memory(read("/proc/meminfo"));
    std::string line;
    std::uint64_t total = 0, available = 0;
    bool have_available = false;
    while (std::getline(memory, line)) {
        std::istringstream row(line);
        std::string name, unit;
        std::uint64_t value;
        if (!(row >> name >> value >> unit) || unit != "kB") continue;
        if (name == "MemTotal:") total = value;
        if (name == "MemAvailable:") { available = value; have_available = true; }
    }
    if (total && have_available && available <= total)
        telemetry.memory = std::to_string((total - available) / 1024) + " / " +
            std::to_string(total / 1024) + " MiB";
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/sys/class/udc", ec), end; !ec && it != end; it.increment(ec)) {
        const auto state = trim(read(it->path().string() + "/state", 128));
        if (!state.empty()) {
            if (!telemetry.usb.empty()) telemetry.usb += '\n';
            telemetry.usb += it->path().filename().string() + '\t' + state;
        }
    }

    refresh_task();
    if (!include_partitions) return;
    partitions.clear();
    ec.clear();
    for (std::filesystem::directory_iterator it("/dev/block/by-name", ec), end;
         !ec && it != end; it.increment(ec)) {
        Partition part;
        part.name = it->path().filename().string();
        if (!partition_name(part.name)) continue;
        part.device = canonical(it->path().string());
        if (!block_device(part.device)) continue;
        part.bytes = device_bytes(*this, part.device);
        part.readonly = trim(read("/sys/class/block/" + std::filesystem::path(part.device).filename().string() +
            "/ro", 64)) != "0";
        auto& cached = filesystems[part.device];
        const double now = monotonic_seconds();
        if (!cached.checked || now - cached.checked >= 10 || cached.bytes != part.bytes) {
            cached = {filesystem(part.device), now, part.bytes};
        }
        part.filesystem = cached.type;
        partitions.push_back(std::move(part));
    }
    std::sort(partitions.begin(), partitions.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::istringstream mounts(read("/proc/mounts"));
    std::string dev, target, type, options;
    while (std::getline(mounts, line)) {
        std::istringstream row(line);
        if (!(row >> dev >> target >> type >> options)) continue;
        const auto path = canonical(mount_field(dev));
        for (auto& part : partitions) {
            if (!path.empty() && path == part.device) {
                part.mount = mount_field(target);
                part.filesystem = type;
            }
        }
    }
}

void Backend::refresh_task() {
    if (const auto child = children.find(this); child != children.end()) {
        int status = 0;
        const auto result = waitpid(child->second, &status, WNOHANG);
        if (result > 0 || (result < 0 && errno == ECHILD)) children.erase(child);
    }
    task = Task{};
    const auto task_json = read(install_preview?std::string(preview_dir)+"task."+std::to_string(preview_step)+".json":
                                 std::string(run)+"task.json",65537);
    if (!task_json.empty() && !decode_task(task_json, task)) {
        task.status = "error";
        task.name = msg::task_status_unavailable;
        task.error = msg::the_task_status_file_is_invalid_check_the_raw;
    }
    if (!install_preview) task.locked = lock_busy() || children.contains(this);
    if (!task.locked && task.status == "running") {
        task.status = "error";
        task.error = msg::the_task_exited_without_a_final_result_check_the;
        if (!task.stages.empty()) {
            auto& stage = task.stages[task.active];
            stage.state = "error";
            for (auto& sub : stage.substeps)
                if (sub.state == "running" || sub.state == "waiting") sub.state = "error";
        }
    }
    if (task.locked && task.id.empty()) {
        task.name = msg::recovery_task;
        task.status = "waiting";
        task.detail = msg::the_task_channel_is_busy_no_task_status_has;
    }
}

bool Backend::start(const std::string& action, const std::string& argument, std::string& error) {
    if (install_preview) { error = msg::no_action_was_run; return false; }
    error.clear();
    refresh();
    if (task.locked) { error = msg::a_task_is_running_wait_for_it_to_finish; return false; }
    if (task.status != "idle" && !task.readonly_restored) {
        error = msg::read_only_protection_has_not_been_confirmed_new_tasks_2;
        return false;
    }
    const Partition* part = nullptr;
    if (action == "hash" || action == "mount" || action == "umount") {
        const auto found = std::find_if(partitions.begin(), partitions.end(),
            [&](const auto& value) { return value.name == argument; });
        if (found == partitions.end() || !found->bytes) {
            error = msg::the_partition_was_not_found_or_its_capacity_is; return false;
        }
        part = &*found;
        if (action == "mount" && (!mountable(part->filesystem) || !part->mount.empty())) {
            error = msg::the_file_system_is_unsupported_or_the_partition_is; return false;
        }
        if (action == "umount" && part->mount != "/mnt/" + part->name) {
            error = msg::only_inspection_mounts_created_by_this_ui_can_be_2; return false;
        }
    } else if (action == "probe") {
        if (!argument.empty()) { error = msg::hardware_collection_does_not_accept_arguments; return false; }
    } else if (action == "bootmode") {
        if (argument != "normal" && argument != "recovery") { error = msg::invalid_boot_mode_choose_again; return false; }
    } else if (action == "power" || action == "reboot") {
        if (argument != "normal" && argument != "bootloader" && argument != "recovery" && argument != "poweroff") {
            error = msg::invalid_power_action_choose_again; return false;
        }
    } else { error = msg::this_action_requires_verified_inputs_from_the_host_and; return false; }
    if (access(helper, R_OK) != 0) { error = msg::this_recovery_image_is_missing_the_task_status_helper; return false; }
    std::error_code ec;
    std::filesystem::create_directories(run, ec);
    if (ec) { error = msg::could_not_create_the_task_status_directory; return false; }
    const int lock = open("/run/overpressure/install.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) close(lock);
        error = msg::could_not_acquire_the_install_lock_check_for_another; return false;
    }
    const int output = open("/run/overpressure/task.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output < 0) { close(lock); error = msg::could_not_create_the_raw_task_log; return false; }
    task = Task{};
    task.id = "ui-start-" + std::to_string(getpid()) + "-" + std::to_string(monotonic_seconds());
    task.status = "running";
    task.name = msg::recovery_task;
    task.detail = msg::starting_the_task;
    task.stages.push_back({task.detail, "running", {}});
    task.locked = true;
    if (!publish(task)) {
        close(output); close(lock); error = msg::could_not_publish_task_status_atomically; return false;
    }
    const pid_t child = fork();
    if (child == 0) {
        if (dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0 ||
            (lock != 9 && dup2(lock, 9) < 0) || fcntl(9, F_SETFD, 0) < 0) _exit(126);
        const int input = open("/dev/null", O_RDONLY);
        if (input >= 0) { dup2(input, STDIN_FILENO); if (input > 2 && input != 9) close(input); }
        if (output > 2 && output != 9) close(output);
        if (lock > 2 && lock != 9) close(lock);
        execl("/bin/sh", "sh", helper, "single", action.c_str(), argument.c_str(), static_cast<char*>(nullptr));
        dprintf(STDERR_FILENO, "task-state exec: %s\n", strerror(errno));
        _exit(127);
    }
    close(output);
    close(lock);
    if (child < 0) {
        error = msg::could_not_start_the_task_process;
        task.status = "error"; task.error = error; task.locked = false;
        publish(task);
        return false;
    }
    children[this] = child;
    if (part) filesystems.erase(part->device);
    return true;
}
}
