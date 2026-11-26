// SPDX-License-Identifier: Unlicense
#include "ui.hpp"
#include "locale.hpp"
#include "install_log_locale.hpp"
#include "log_progress_policy.hpp"
#include "progress_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>
#include <charconv>
#include <ctime>
#include <cstdio>
#include <tuple>

namespace p4ui {
namespace {

constexpr int volume_up = 115, volume_down = 114, power_key = 116;
constexpr double sheet_x = 360, sheet_y = 440;
constexpr int row_height = 92, button_height = 104;
constexpr int log_height = log_viewport_height;
constexpr double reader_delay=.34,reader_ramp=.95,reader_start_rate=4,reader_max_rate=50,reader_coast_time=.36;
constexpr double reader_catch_time=.12;
constexpr std::size_t text_limit = 1024 * 1024;
constexpr std::array service_names{
    "udhcpd", "dropbear", "telnetd", "httpd", "serial", "usb-watch", "recovery-watch", "recovery-ui"};

struct Report {
    const char* id;
    const char* name;
    const char* file;
    const char* extra = nullptr;
};

constexpr std::array reports{
    Report{"system", msg::system_version, "system.txt"},
    Report{"cpu", msg::cpu_memory, "cpu.txt", "/proc/meminfo"},
    Report{"dt", msg::device_tree, "devicetree.txt", "devicetree.dts"},
    Report{"kernel", msg::kernel_configuration_modules, "proc-modules.txt", "devices-deferred.txt"},
    Report{"storage", msg::block_devices_partitions, "storage.txt"},
    Report{"display", msg::gpu_display, "display.txt", "gpu.txt"},
    Report{"input", msg::physical_inputs, "input.txt"},
    Report{"power", msg::power_status, "power.txt"},
    Report{"thermal", msg::thermal_nodes, "thermal.txt"},
    Report{"led", msg::leds, "leds.txt"},
    Report{"usb", msg::usb_serial, "usb.txt"},
    Report{"net", msg::network_nodes, "net.txt"},
    Report{"sound", msg::audio_nodes, "sound.txt"},
    Report{"video", msg::video_nodes, "video.txt"},
    Report{"remoteproc", msg::remote_processors, "remoteproc.txt"},
};

bool known_page(const std::string& id) {
    constexpr std::array ids{
        "LANGUAGE", "BOOT", "BOOT_ERROR", "CURRENT_TASK", "HOME", "STATUS",
        "HARDWARE", "SERVICES", "DEADLINE", "HOST", "MAINT", "BUILD_MENU",
        "STOCK_MENU", "RESTORE_MENU", "INPUTS", "TASK_DETAIL", "REVIEW",
        "CONFIRM", "RUNNING", "SUCCESS", "ERROR", "UNAVAILABLE", "STORAGE",
        "PARTITION", "HASH", "MOUNT", "DIAG", "REPORT", "LOGS", "LOG_VIEW",
        "POWER", "BOOT_MODE", "DISPLAY", "LAYER_EDIT", "EXIT", "SETTINGS"};
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::string trimmed(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string observed(const std::string& value) {
    return value.empty() ? msg::unknown_2 : value;
}

std::string preference_storage(const std::string& storage) {
    if (storage == "data") return msg::data_partition;
    if (storage == "cache") return msg::cache_partition;
    if (storage == "none") return msg::no_storage_selected;
    return msg::unknown_storage_state;
}

// Preferences retain the original diagnostic; only presentation classifies it.
std::string preference_notice(const std::string& error, bool saving) {
    if (error.empty()) return {};
    const auto has = [&](const char* text) { return error.find(text) != std::string::npos; };
    std::string detail = msg::the_save_result_could_not_be_verified_check_storage;
    if (has("BLKROSET ro") || has("not read-only") || has("只读恢复"))
        detail = msg::storage_is_not_confirmed_to_be_read_only_again;
    else if (has("超时") || has("timeout"))
        detail = msg::waiting_for_the_save_timed_out_it_may_still;
    else if (has("正在运行") || has("install.lock") || has("flock"))
        detail = msg::another_task_is_using_settings_storage_wait_for_it;
    else if (has("尚在缓存"))
        detail = msg::language_settings_are_still_on_the_cache_partition_save;
    else if (has("语言暂不支持") || has("所选语言") || has("saved locale"))
        detail = msg::this_language_is_not_supported_choose_simplified_chinese_or;
    else if (has("version"))
        detail = msg::this_settings_file_version_is_unsupported_existing_settings_will;
    else if (has("readback"))
        detail = msg::the_settings_read_back_did_not_pass_verification_the;
    else if (has("JSON") || has("preferences fields") || has("preferences size") ||
        has("preferences content") || has("size limit"))
        detail = msg::the_settings_file_or_save_result_is_invalid_recovery;
    else if (has("No such file") || has("resolve ") || has("filesystem unknown"))
        detail = msg::no_settings_storage_is_available_check_the_data_or;
    else if (has("needs recovery") || has("ext4") || has("filesystem") || has("label"))
        detail = msg::the_settings_file_system_did_not_pass_safety_checks;
    else if (has("No space") || has("write ") || has("sync ") || has("rename "))
        detail = msg::the_settings_file_could_not_be_written_or_synced;
    else if (has("Permission denied") || has("unsafe") || has("root-owned") ||
        has("directory") || has("symlink"))
        detail = msg::the_settings_path_or_permissions_did_not_pass_safety;
    else if (has("mount") || has("mounted"))
        detail = msg::settings_storage_could_not_be_mounted_or_unmounted_check;
    return std::string(saving ? msg::could_not_confirm_that_language_settings_were_saved :
        msg::saved_language_settings_could_not_be_read) + "\n" + detail;
}

std::string battery_status(const std::string& status) {
    if (status == "Charging") return msg::charging;
    if (status == "Discharging") return msg::discharging;
    if (status == "Full") return msg::fully_charged;
    if (status == "Not charging") return msg::not_charging;
    return msg::unknown;
}

std::string task_status(const std::string& status) {
    if (status == "running" || status == "starting") return msg::in_progress;
    if (status == "waiting" || status == "queued" || status == "pending") return msg::waiting;
    if (status == "done" || status == "success") return msg::completed_2;
    if (status == "error" || status == "failed") return msg::failed;
    if (status == "skipped") return msg::skipped_2;
    if (status == "idle") return msg::idle;
    return msg::status_unknown;
}

std::string decimal(double value, int precision = 1) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string rate_text(const std::optional<double>& rate,int mode=0) {
    if (!rate || !std::isfinite(*rate) || *rate < 0) return mode==2?"-- bit/s":"-- B/s";
    constexpr std::array binary{"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s", "PiB/s", "EiB/s"};
    constexpr std::array decimal_units{"B/s","kB/s","MB/s","GB/s","TB/s","PB/s","EB/s"};
    constexpr std::array bits{"bit/s","kbit/s","Mbit/s","Gbit/s","Tbit/s","Pbit/s","Ebit/s"};
    const auto& units=mode==2?bits:mode==1?decimal_units:binary;
    double value = *rate*(mode==2?8:1);
    std::size_t unit = 0;
    // Keep three significant digits within the status slot, including rounding
    // across the next unit. The underlying sampled byte rate remains unchanged.
    while (value >= 999.5 && unit + 1 < units.size()) { value /= mode==0?1024:1000; ++unit; }
    return decimal(value, unit && value < 10 ? 1 : 0) + units[unit];
}

std::string formatted(std::string_view key,bool en,std::initializer_list<std::string> values);

std::string clock_label(const Telemetry& telemetry,const Preferences& p) {
    const bool show_time=p.visible&64,show_date=p.visible&128;
    if (!show_time&&!show_date) return {};
    if (!telemetry.time_valid) return show_time?msg::not_synced:msg::time_not_synced;
    std::tm local{};
    time_t epoch=telemetry.epoch;
    if (p.zone && epoch) {
        // Fixed, packaged TZif data is loaded separately for the supported host zones.
        if (!display_clock(p.zone,epoch,local)) return msg::time_not_synced;
    } else if (epoch) {
        if (!localtime_r(&epoch,&local)) return msg::time_not_synced;
    } else {
        if (std::sscanf(telemetry.clock.c_str(),"%d:%d:%d",&local.tm_hour,&local.tm_min,&local.tm_sec)!=3 ||
            std::sscanf(telemetry.date.c_str(),"%d-%d-%d",&local.tm_year,&local.tm_mon,&local.tm_mday)!=3)
            return msg::time_not_synced;
        local.tm_year-=1900;--local.tm_mon;
    }
    char buffer[96];
    std::strftime(buffer,sizeof buffer,p.visible&256?"%H:%M:%S":"%H:%M",&local);
    std::string time=buffer;
    if(p.hour==12){
        std::strftime(buffer,sizeof buffer,p.visible&256?":%M:%S":":%M",&local);
        time=std::to_string(local.tm_hour%12?local.tm_hour%12:12)+buffer;
        time=formatted(local.tm_hour<12?msg::time_morning_arg0:msg::time_afternoon_arg0,p.locale=="en",{time});
    }
    constexpr std::array formats{"%Y-%m-%d","%Y/%m/%d","%d/%m/%Y","%m/%d/%Y"};
    std::strftime(buffer,sizeof buffer,formats[std::clamp(p.date_style,0,3)],&local);
    const std::string date=buffer;
    if (!show_time) return date;
    if (!show_date) return time;
    return p.date_first?date+"  "+time:time+"  "+date;
}

std::string formatted(std::string_view key,bool en,std::initializer_list<std::string> values) {
    const auto text=localized(key,en);
    std::string out;
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]=='{'&&i+2<text.size()&&text[i+2]=='}'&&text[i+1]>='0'&&text[i+1]<='9'&&
            static_cast<std::size_t>(text[i+1]-'0')<values.size()){
            out+=values.begin()[text[i+1]-'0'];i+=2;
        }else out+=text[i];
    }
    return out;
}

std::string bytes_text(std::uint64_t bytes,bool en) {
    if (!bytes) return msg::capacity_unknown;
    const double count = static_cast<double>(bytes);
    if (bytes >= (std::uint64_t{1} << 30)) return decimal(count / (1ULL << 30)) + " GiB";
    if (bytes >= (std::uint64_t{1} << 20)) return decimal(count / (1ULL << 20)) + " MiB";
    return formatted(msg::arg0_b,en,{std::to_string(bytes)});
}

std::vector<std::string> lines_of(const std::string& value) {
    std::vector<std::string> result;
    std::istringstream input(value);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result.push_back(std::move(line));
    }
    if (result.empty()) result.emplace_back();
    return result;
}

std::string uptime_label(std::string_view minutes,bool en) {
    if(minutes.empty())return std::string(localized(msg::unknown,en));
    std::uint64_t value=0;
    const auto [last,err]=std::from_chars(minutes.data(),minutes.data()+minutes.size(),value);
    if(err!=std::errc{}||last!=minutes.data()+minutes.size())return std::string(minutes);
    return formatted(msg::arg0_h_arg1_min,en,{std::to_string(value/60),std::to_string(value%60)});
}

std::string usb_label(const std::string& raw,bool en) {
    constexpr std::pair<std::string_view,std::string_view> keys[]={
        {"not attached",msg::disconnected},{"attached",msg::connected},{"powered",msg::powered},
        {"reconnecting",msg::reconnecting},{"unauthenticated",msg::not_authenticated},
        {"default",msg::default_state},{"addressed",msg::address_assigned},
        {"configured",msg::configured},{"suspended",msg::suspended}};
    std::string result;
    for(const auto& row:lines_of(raw)){
        const auto field=row.find('\t');
        if(field==row.npos||field==0)continue;
        const auto state=std::string_view(row).substr(field+1);
        const auto tag=std::ranges::find_if(keys,[&](const auto& pair){return pair.first==state;});
        if(!result.empty())result+=" · ";
        result+=formatted(msg::controller_usb_state,en,{row.substr(0,field),
            std::string(localized(tag==std::end(keys)?std::string_view(msg::usb_status_unknown):tag->second,en))});
    }
    return result.empty()?std::string(localized(msg::usb_status_unknown,en)):result;
}

std::string field(const std::string& content, const std::string& name, char separator = '=') {
    for (const auto& line : lines_of(content)) {
        const auto at = line.find(separator);
        if (at != std::string::npos && trimmed(line.substr(0, at)) == name)
            return trimmed(line.substr(at + 1));
    }
    return {};
}

bool valid_partition_name(const std::string& name) {
    return !name.empty() && name.size() <= 128 &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }) && name != "." && name != "..";
}

const Partition* partition(const Backend& backend, const std::string& name) {
    if (!valid_partition_name(name)) return nullptr;
    const auto it = std::find_if(backend.partitions.begin(), backend.partitions.end(),
        [&](const Partition& item) { return item.name == name; });
    return it == backend.partitions.end() ? nullptr : &*it;
}

bool mountable(const Partition& item) {
    return item.filesystem == "ext2" || item.filesystem == "ext3" ||
        item.filesystem == "ext4" || item.filesystem == "f2fs" ||
        item.filesystem == "vfat" || item.filesystem == "erofs" ||
        item.filesystem == "squashfs";
}

bool task_present(const Task& task) {
    return !task.id.empty() || !task.name.empty() || !task.stages.empty() ||
        task.status != "idle";
}

bool task_running(const Task& task) {
    return task.locked || task.status == "running" || task.status == "waiting" ||
        task.status == "starting" || task.status == "queued";
}

bool task_failed(const Task& task) {
    return task.status == "error" || task.status == "failed";
}

bool task_succeeded(const Task& task) {
    return task.status == "success" || task.status == "done";
}

bool installation_task(const Task& task) {
    return task.kind == "host" || task.kind == "headset" ||
        (task.kind.empty() && (task.name=="System installation" || task.name=="系统安装") &&
         (task.stages.size()==7 || task.stages.size()==9));
}

std::string channel_reason(const Backend& backend) {
    if (task_running(backend.task))
        return msg::a_task_is_already_active_wait_for_it_to;
    if (task_present(backend.task) && backend.task.status != "idle" &&
        !backend.task.readonly_restored)
        return msg::read_only_protection_has_not_been_confirmed_new_tasks;
    return {};
}

struct Request {
    std::string name = msg::action_unavailable, label = msg::run, module, effect;
    std::vector<std::string> inputs, steps;
    std::string action, argument, reason;
};

Request request(const Backend& backend, const std::string& key, bool en) {
    const auto tr=[&](std::string_view value){return std::string(localized(value,en));};
    Request result;
    const auto colon = key.find(':');
    const std::string operation = key.substr(0, colon);
    const std::string argument = colon == std::string::npos ? "" : key.substr(colon + 1);
    if (operation == "probe" && argument.empty()) {
        result.name = msg::collect_hardware_info;
        result.label = msg::start_collection;
        result.module = "pico4-probe";
        result.effect = msg::updates_recovery_s_hardware_reports_does_not_test_audio;
        result.inputs = {msg::reads_proc_sysfs_device_tree, msg::output_run_overpressure_report,
            msg::does_not_enable_the_hardware_functions_listed_in_the, msg::collects_information_without_testing_functions};
        result.steps = {msg::read_system_nodes, msg::organize_report_categories, msg::write_reports};
        result.action = "probe";
    } else if (operation == "hash") {
        result.name = msg::compute_partition_sha_256;
        result.label = msg::compute_hash;
        result.module = "pico4-install hash NAME BYTES";
        result.effect = msg::reads_the_partition_without_writing_and_computes_sha_256;
        result.steps = {msg::check_partition_size, msg::compute_sha_256_read_only, msg::record_hash};
        const auto* item = partition(backend, argument);
        result.inputs = {formatted(msg::target_arg0,en,{argument}), formatted(msg::byte_count_arg0,en,
            {item && item->bytes ? std::to_string(item->bytes) : tr(msg::unknown)}),
            msg::access_read_only, msg::no_expected_hash_was_supplied_for_comparison};
        if (!item || !item->bytes) result.reason = msg::the_partition_could_not_be_found_or_its_byte;
        result.action = "hash";
        result.argument = argument;
    } else if (operation == "mount" || operation == "umount") {
        const bool unmount = operation == "umount";
        result.name = unmount ? msg::unmount_inspection_mount : msg::mount_read_only;
        result.label = result.name;
        result.module = unmount ? msg::umount_inspection_mount : "pico4-mount-ro";
        result.effect = unmount ? msg::unmounts_the_inspection_mount_without_force_or_lazy_unmount :
            msg::mounts_the_selected_partition_read_only_ext4_journal_replay;
        result.steps = {unmount ? msg::check_current_mount : msg::check_file_system,
            unmount ? msg::unmount_without_force : msg::mount_read_only, msg::read_mount_status};
        const auto* item = partition(backend, argument);
        result.inputs = {formatted(msg::target_arg0,en,{argument}),
            formatted(msg::file_system_arg0,en,{tr(item ? observed(item->filesystem) : msg::unknown)}),
            formatted(msg::existing_mount_arg0,en,{item && !item->mount.empty() ? item->mount : tr(msg::no_mount_detected)}),
            msg::no_file_browser_editing_or_repair_is_available_here};
        if (!item || !item->bytes) result.reason = msg::the_partition_could_not_be_found_or_its_capacity;
        else if (unmount && item->mount != "/mnt/" + item->name)
            result.reason = msg::only_inspection_mounts_created_by_this_ui_can_be;
        else if (!unmount && !item->mount.empty()) result.reason = msg::the_partition_is_already_mounted_its_mount_will_not;
        else if (!unmount && !mountable(*item)) result.reason = msg::this_file_system_is_not_supported_for_read_only;
        result.action = operation;
        result.argument = argument;
    } else if ((operation == "normal" || operation == "recovery") && argument.empty()) {
        result.name = operation == "normal" ? msg::set_next_boot_to_normal : msg::set_next_boot_to_recovery;
        const std::string choice = operation == "normal" ? msg::reboot_normally : msg::recovery;
        result.label = operation=="normal" ? msg::set_normal_boot : msg::set_recovery_boot;
        result.module = "pico4-install bootmode " + operation;
        result.effect = operation == "normal" ?
            msg::clears_the_recovery_boot_request_the_write_must_pass :
            msg::sets_the_next_boot_to_recovery_after_writing_misc;
        result.inputs = {msg::target_misc_bcb, formatted(msg::boot_mode_arg0,en,{tr(choice)}),
            msg::the_write_is_checked_by_reading_it_back, msg::does_not_restart_now};
        result.steps = {msg::check_bcb, result.label, msg::read_back_misc, msg::restore_read_only_protection};
        if (!partition(backend, "misc")) result.reason = msg::the_misc_partition_was_not_found;
        result.action = "bootmode";
        result.argument = operation;
    } else if (operation == "power" &&
        (argument == "normal" || argument == "bootloader" ||
         argument == "recovery" || argument == "poweroff")) {
        result.name = argument == "normal" ? msg::reboot_normally :
            argument == "bootloader" ? msg::reboot_to_bootloader :
            argument == "recovery" ? msg::reboot_to_recovery : msg::shut_down;
        result.label = result.name;
        result.module = msg::pico4_reboot_supported_targets;
        result.effect = argument == "normal" ?
            msg::clears_the_recovery_boot_request_then_attempts_a_normal :
            msg::ends_this_recovery_session_the_action_is_available_only;
        result.inputs = {formatted(msg::target_arg0,en,{tr(result.name)}), msg::requirement_no_conflicting_tasks,
            msg::normal_boot_no_reboot_if_misc_readback_fails, msg::long_press_power_handled_by_the_kernel};
        result.steps = argument == "normal" ?
            std::vector<std::string>{msg::write_normal, msg::read_back_misc, msg::restore_read_only_protection, msg::reboot_normally_2} :
            std::vector<std::string>{msg::check_for_conflicting_tasks, msg::sync_and_exit_recovery};
        if (argument == "normal" && !partition(backend, "misc"))
            result.reason = msg::the_misc_partition_was_not_found_the_normal_boot;
        result.action = "power";
        result.argument = argument;
    } else {
        struct Maintenance {
            const char* key;
            const char* name;
            const char* module;
            const char* effect;
        };
        constexpr std::array maintenance{
            Maintenance{"install", msg::install_system_images_2, "pico4-install write boot / super",
                msg::overwrites_boot_and_super_replacing_the_existing_android_system},
            Maintenance{"build", msg::build_on_this_device, "pico4-install build / build-file",
                msg::writes_build_output_and_logs_to_pico4_data_does},
            Maintenance{"format", msg::format_for_linux_data, "pico4-install format-data --size …",
                msg::formats_the_entire_userdata_partition_existing_apps_and_user},
            Maintenance{"keys", msg::install_public_keys_install_id, "pico4-install keys --size … --id …",
                msg::updates_the_installed_system_s_authorized_ssh_keys_and},
            Maintenance{"stock", msg::update_device_system_assets, "pico4-install stock",
                msg::replaces_the_asset_copy_only_after_all_checks_pass},
            Maintenance{"stockhash", msg::verify_asset_copy, "pico4-install stock-hash --size …",
                msg::computes_and_compares_hashes_for_the_existing_copy_files},
            Maintenance{"restore", msg::restore_the_factory_system, msg::host_restore_installed_pico4_install_write,
                msg::overwrites_boot_and_super_to_restore_the_factory_system},
            Maintenance{"wipeuser", msg::clear_start_of_userdata_2, "pico4-install wipe userdata --size …",
                msg::zeros_the_first_64_mib_of_userdata_making_the},
            Maintenance{"wipemeta", msg::clear_start_of_metadata_2, "pico4-install wipe metadata --size …",
                msg::zeros_the_start_of_the_partition_up_to_the},
        };
        const auto it = std::find_if(maintenance.begin(), maintenance.end(),
            [&](const Maintenance& item) { return item.key == key; });
        if (it != maintenance.end()) {
            result.name = it->name;
            result.module = it->module;
            result.effect = it->effect;
            result.inputs = {msg::source_no_usable_install_inputs,
                msg::capacity_byte_count_sha_no_verification_result,
                msg::keys_sources_and_device_backup_provide_as_required_for,
                msg::provide_inputs_through_the_existing_host_workflow_this_page};
            result.reason = msg::required_inputs_target_sizes_or_verification_results_are_missing;
        } else result.reason = msg::this_action_is_unsupported_and_will_not_run;
    }
    const auto busy = channel_reason(backend);
    if (!busy.empty()) result.reason = busy;
    return result;
}

std::vector<Stage> stages_of(const Task& task) {
    if (!task.stages.empty()) return task.stages;
    return {{msg::check_inputs_and_device, "pending", {}, "check"},{msg::write_and_verify_boot, "pending", {}, "boot"},
        {msg::write_and_verify_super, "pending", {}, "super"},{msg::prepare_user_data, "pending", {}, "data"},
        {msg::public_keys_and_install_id, "pending", {}, "keys"},{msg::system_assets, "pending", {}, "assets"},
        {msg::select_normal_boot, "pending", {}, "finish"}};
}

std::string stage_summary(const Task& task,bool en=false) {
    if (!task_present(task)) return msg::installation_has_not_started_waiting_for_the_host;
    if (task.status=="waiting"&&task.active>=0&&!task.stages.empty()&&
        static_cast<std::size_t>(task.active)<task.stages.size()&&
        std::all_of(task.stages.begin(),task.stages.end(),[](const Stage& s){return s.state=="done"||s.state=="skipped";}))
        return msg::install_steps_complete_first_boot_not_yet_verified;
    const auto stages = stages_of(task);
    for (const auto& stage : stages) {
        if (stage.state != "running" && stage.state != "waiting" && stage.state != "error")
            continue;
        const auto* words=progress_ui::progress_words(progress_ui::stage_words,stage.id);
        const std::string name=words?std::string(localized(words->full,en)):install_log::message(stage.name,en);
        if (stage.state == "error") return formatted(msg::arg0_failed,en,{name});
        if (stage.fraction && std::isfinite(*stage.fraction) &&
            *stage.fraction >= 0 && *stage.fraction <= 1)
            return formatted(msg::arg0_arg1_of_this_stage,en,{name,decimal(std::floor(*stage.fraction * 100),0)});
        return formatted(msg::arg0_in_progress_total_unknown,en,{name});
    }
    if (task_failed(task)) return msg::task_failed_check_the_error_details_and_logs;
    if (task_succeeded(task)) return msg::the_task_process_exited_check_the_task_result;
    return msg::waiting_for_stage_status;
}

// Legacy wrapping for service summaries; log and report text use wrap_log.
std::vector<std::string> wrap(const std::string& value, double width, double size) {
    std::vector<std::string> output;
    for (const auto& paragraph : lines_of(value)) {
        std::string line;
        double advance = 0;
        for (std::size_t i = 0; i < paragraph.size();) {
            const auto lead = static_cast<unsigned char>(paragraph[i]);
            std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
            if (length > paragraph.size() - i) length = 1;
            for (std::size_t j = 1; j < length; ++j) {
                if ((static_cast<unsigned char>(paragraph[i + j]) & 0xc0) != 0x80) {
                    length = 1;
                    break;
                }
            }
            const double glyph = lead == '\t' ? size * 4 : lead == 0 ? size * 2 : size;
            if (!line.empty() && advance + glyph > width) {
                output.push_back(std::move(line));
                line.clear();
                advance = 0;
            }
            if (lead == '\t') line += "    ";
            else if (lead == 0) line += "\\0";
            else line.append(paragraph, i, length);
            advance += glyph;
            i += length;
        }
        output.push_back(std::move(line));
    }
    return output;
}

// Prepare log lines exactly once, with the same font advances as Painter.
// Offsets are in the sanitized entry, so a width change can keep its reading position.
std::vector<LogLine> wrap_log(std::string_view value, double width,
    const std::function<LogGlyph(std::string_view)>& measure) {
    std::vector<LogLine> output;
    std::string clean;
    clean.reserve(value.size());
    for(unsigned char c : value) {
        if(c=='\t') clean+="    ";
        else if(c==0) clean+="\\0";
        else if(c>=32 || c=='\n') clean+=static_cast<char>(c);
    }
    const std::string_view text=clean;
    for(std::size_t start=0,i=0,last_break=0;;) {
        double advance=0;
        i=start;last_break=start;
        while(i<text.size()&&text[i]!='\n') {
            const auto lead=static_cast<unsigned char>(text[i]);
            std::size_t n=lead>=0xc2&&lead<0xe0?2:lead>=0xe0&&lead<0xf0?3:lead>=0xf0&&lead<=0xf4?4:1;
            if(n>text.size()-i)n=1;
            for(std::size_t j=1;j<n;++j)
                if((static_cast<unsigned char>(text[i+j])&0xc0)!=0x80){n=1;break;}
            const auto glyph=text.substr(i,n);
            const auto m=measure?measure(glyph):LogGlyph{log_font_px,log_font_px};
            if(i>start && advance+std::max(m.advance,m.right)>width)break;
            advance+=m.advance;
            i+=n;
            if(lead==' '||lead=='/'||lead=='-')last_break=i;
        }
        if(i==text.size()||text[i]=='\n'){
            output.push_back({std::string(text.substr(start,i-start)),start});
            if(i==text.size()) break;
            start=i+1;
        }else{
            // Prefer natural boundaries near the end; break long tokens without losing bytes.
            if(last_break>start && i-last_break<=16 && (i-last_break)*4<=i-start)i=last_break;
            output.push_back({std::string(text.substr(start,i-start)),start});
            start=i;
        }
    }
    return output;
}

std::string install_stage_detail(std::string_view detail, bool en) {
    return install_log::message(detail,en);
}

void localize_install_stage_line(std::string& line,bool en) {
    const std::string_view view(line);
    if (view.starts_with('[')) {
        const auto close=view.find("] ");
        if(close!=view.npos){
            constexpr std::pair<std::string_view,std::string_view> tags[]={
                {"running",msg::in_progress},{"waiting",msg::waiting},{"done",msg::completed},
                {"skipped",msg::skipped},{"error",msg::failed}};
            const auto tag=view.substr(1,close-1);
            for(const auto& [source,key]:tags){
                if(tag!=source)continue;
                line="["+std::string(en?source:localized(key,false))+"] "+install_stage_detail(view.substr(close+2),en);
                return;
            }
            const auto body=view.substr(close+2);
            if(body.starts_with("install: ")){
                line=std::string(view.substr(0,close+2))+install_stage_detail(body,en);
                return;
            }
        }
    }
    line=install_log::line(view,en);
}

std::vector<std::string> log_lines(const Backend& backend, const std::string& source, bool en) {
    if (source == "install-live") {
        auto boot=backend.logs("/run/overpressure/boot.log");
        if(!en)
            for(auto& line:boot)if(line.find("] install: ")!=std::string::npos)localize_install_stage_line(line,false);
        if (backend.install_preview || installation_task(backend.task)) {
            auto task=backend.logs("/run/overpressure/task.log");
            if(!en)for (auto& line : task) localize_install_stage_line(line,false);
            boot.insert(boot.end(),std::make_move_iterator(task.begin()),std::make_move_iterator(task.end()));
        }
        if(boot.size()>2048)boot.erase(boot.begin(),boot.end()-2048);
        return boot;
    }
    if (source == "task" || source == "/run/overpressure/task.log") {
        auto result=backend.logs("/run/overpressure/task.log");
        if(!en)for(auto& line:result)localize_install_stage_line(line,false);
        return result;
    }
    if (source == "kernel") return backend.logs("/run/overpressure/report/dmesg.txt");
    if (source == "boot") return backend.logs("/run/overpressure/boot.log");
    if (source == "build") return backend.logs("/run/overpressure/report/build.txt");
    if (source != "services" && source != "pstore") return backend.logs(source);
    std::vector<std::string> result;
    const auto append = [&](const std::string& path, const std::vector<std::string>& entries) {
        for (const auto& entry : entries) result.push_back("[" + path + "] " + entry);
    };
    if (source == "services") {
        for (const auto* name : service_names) {
            std::string path = "/run/overpressure/svc-" + std::string(name) + ".log";
            auto content = backend.read(path, text_limit);
            if (content.empty()) {
                path = "/run/overpressure/report/svc-" + std::string(name) + ".log";
                content = backend.read(path, text_limit);
            }
            if (!content.empty()) append(path, lines_of(content));
        }
    } else {
        for (const auto* type : {"console-ramoops-", "pmsg-ramoops-"}) {
            for (int i = 0; i < 16; ++i) {
                const std::string path = "/sys/fs/pstore/" + std::string(type) + std::to_string(i);
                append(path, backend.logs(path));
            }
        }
    }
    return result;
}

auto entry_at(const LogDocument& document,int row) {
    return std::prev(std::upper_bound(document.entries.begin(),document.entries.end(),row,
        [](int index,const LogEntry& entry){return index<entry.first_row;}));
}

const LogDocument& log_document(App& app) {
    auto& cached=app.log_layout;
    const auto width=app.log_width.text();
    const bool en=app.settings().locale=="en";
    const bool same_layout=cached.source==app.log_source&&cached.width==width&&cached.english==en;
    const bool watched=app.live_log_events&&(log_progress::watches_task_log(app)||log_progress::watches_boot_log(app));
    if(same_layout&&(watched?!app.live_log_dirty:
        app.animation_phase>0&&app.animation_phase-cached.sampled_at<1))return cached;
    if(watched)app.live_log_dirty=false;
    auto input=log_lines(app.backend,app.log_source,en);
    if(same_layout&&cached.input==input){
        cached.sampled_at=app.animation_phase;
        if(app.log_source=="install-live"&&app.follow_install_log){
            app.log_top=cached.max_top();
            app.log_cursor=std::max(0,cached.row_count-1);
        }
        return cached;
    }
    LogDocument result;
    result.source=app.log_source;result.width=width;result.english=en;result.input=std::move(input);
    result.sampled_at=app.animation_phase;
    auto first=result.input.begin();
    if(same_layout){
        // Reuse measured rows on append; truncation and rotation rebuild the document.
        const auto prefix=std::mismatch(cached.input.begin(),cached.input.end(),first,result.input.end());
        if(prefix.first==cached.input.end()){
            first=prefix.second;
            result.entries=std::move(cached.entries);
            result.row_count=cached.row_count;
        }
    }
    for (;first!=result.input.end();++first) {
        const auto& value=*first;
        auto lines = wrap_log(value,width-2,app.measure_log_glyph);
        if (lines.empty()) continue;
        if (static_cast<std::size_t>(std::numeric_limits<int>::max()-result.row_count)<lines.size()) break;
        const int rows=static_cast<int>(lines.size());
        result.entries.push_back({std::move(lines),result.row_count});
        result.row_count+=rows;
    }
    // Track both the viewed and selected bytes independently when wrapping changes.
    if(cached.source==app.log_source&&cached.width!=width&&cached.row_count&&cached.input==result.input){
        auto reflow=[&](int row){
            row=std::clamp(row,0,cached.row_count-1);
            const auto old=entry_at(cached,row);
            const auto index=static_cast<std::size_t>(old-cached.entries.begin());
            if(index>=result.entries.size())return row;
            const auto byte=old->lines[row-old->first_row].offset;
            const auto& entry=result.entries[index];
            const auto at=std::upper_bound(entry.lines.begin(),entry.lines.end(),byte,
                [](std::size_t offset,const LogLine& line){return offset<line.offset;});
            return entry.first_row+static_cast<int>(std::prev(at)-entry.lines.begin());
        };
        app.log_top=reflow(app.log_top);
        app.log_cursor=reflow(app.log_cursor);
    }
    if(app.log_source=="install-live"&&app.follow_install_log){
        app.log_top=result.max_top();
        app.log_cursor=std::max(0,result.row_count-1);
    }
    cached=std::move(result);
    return cached;
}

std::string log_title(const std::string& source) {
    if (source == "/run/overpressure/boot.log" || source == "boot") return msg::boot_log;
    if (source == "kernel") return msg::kernel_log;
    if (source == "services") return msg::service_logs;
    if (source == "pstore") return msg::pstore_records;
    if (source == "task" || source == "/run/overpressure/task.log") return msg::task_output;
    if (source == "build") return msg::build_logs;
    return msg::logs;
}

struct Scene {
    App& app;
    double x = sheet_x, y = sheet_y;
    int row = 0;
    bool has_back = false, paged = false;
    std::vector<std::pair<std::size_t, std::size_t>> buttons{};

    void box(double px, double py, double w, double h, Color color = view,
        Layer layer = Layer::background, Color border = rule, double stroke = 0,
        double radius = 10, Kind kind = Kind::box) {
        Node node;
        node.kind = kind;
        node.layer = layer;
        node.x = x + px; node.y = y + py; node.w = w; node.h = h;
        node.color = color; node.border = border; node.border_width = stroke;
        node.radius = radius;
        app.nodes.push_back(std::move(node));
    }

    void text(double px, double py, double w, double h, const std::string& value,
        Color color = ink, double size = 29.333, bool bold = false, bool right = false,
        bool center = false, bool middle = false) {
        Node node;
        node.kind = Kind::text; node.layer = Layer::text;
        node.x = x + px; node.y = y + py; node.w = w; node.h = h;
        const bool en=app.settings().locale=="en";
        const auto translated=localized(value,en);
        if(translated==value && value.starts_with("ui.") && value.find('\n')!=std::string::npos) {
            for(const auto& line:lines_of(value)) {
                if(!node.value.empty())node.value+='\n';
                node.value+=localized(line,en);
            }
        }else node.value=translated;
        node.color = color; node.size = size;
        node.bold = bold; node.right = right; node.center = center; node.middle = middle;
        app.nodes.push_back(std::move(node));
    }

    void icon(double px, double py, const std::string& name, Color color = ink, double size = 44) {
        Node node;
        node.kind = Kind::icon; node.layer = Layer::icon;
        node.x = x + px; node.y = y + py; node.w = size; node.h = size;
        node.value = name; node.color = color;
        app.nodes.push_back(std::move(node));
    }

    bool selected(int index) const { return app.focus == index && !app.dialog && app.choice.empty(); }
    Color highlight(bool chosen,Color rest) const {
        return chosen ? (app.power_held?Color{85/255.,161/255.,205/255.}:selection) : rest;
    }

    int bind(std::function<void()> fn, bool enabled = true) {
        const int index = static_cast<int>(app.actions.size());
        app.actions.push_back(enabled ? std::move(fn) : std::function<void()>{});
        return index;
    }

    void page_background() {
        // Flat fill for page transitions; the surrounding canvas uses the same color.
        box(0,0,1440,1280,window,Layer::background,window,0,0);
    }

    void title(const std::string& name, const std::string& subtitle, bool back = true,
        bool home = false) {
        page_background();
        box(2, 2, 1436, 196, titlebar);
        // Reserve the upper-right header area for the live traffic row.
        text(36, 76, 1032, 76, name, ink, 52, true);
        text(36, 154, 1330, 46, subtitle, muted);
        box(36, 214, 1368, 2, rule, Layer::background, rule, 0, 0);
        has_back = back;
        if (!back) return;
        const int index = bind([this_app = &app, home] {
            if (!home) { this_app->back(); return; }
            this_app->stack.clear();
            this_app->page = "HOME";
            this_app->argument.clear();
            this_app->focus = 0;
            this_app->editing = -1;
            this_app->dialog = false;
        });
        const bool chosen = selected(index);
        box(36, 244, 276, 62, highlight(chosen,window), Layer::button,
            chosen ? selection : rule, 2);
        icon(52, 255, "back", chosen ? selected_ink : ink, 34);
        text(36, 244, 276, 62, home ? msg::back_to_home : msg::back_2,
            chosen ? selected_ink : ink, 25, false, false, true, true);
    }

    void item(const std::string& label, const std::string& description,
        std::function<void()> fn, const std::string& glyph = "next", bool enabled = true,
        int edit = -1, bool dropdown=false, std::optional<bool> toggle=std::nullopt) {
        const int index = bind(std::move(fn), enabled);
        const int position = row++;
        const int first = paged ? std::max(0, (app.focus - (has_back ? 1 : 0)) / 7) * 7 : 0;
        if (paged && (position < first || position >= first + 7)) return;
        const double py = (has_back ? 332 : 264) + (position - first) * 112;
        const bool chosen = selected(index) && app.editing < 0;
        const bool editing = app.editing == edit && edit >= 0;
        const auto color = chosen ? selected_ink : enabled ? ink : muted;
        box(36, py, 820, row_height, highlight(chosen,editing ? view : alternate),
            Layer::button, chosen || editing ? selection : alternate, editing ? 4 : 2);
        if(!glyph.empty()) icon(56, py + (row_height - 44) / 2, glyph, color);
        const auto tx=glyph.empty()?60:120;
        text(tx, py + (description.empty() ? 16 : 6), toggle?620:786-tx, 48, label, color);
        if (!description.empty())
            text(tx, py + 50, 786-tx, 38, description, chosen ? selected_ink : muted, 25);
        if (toggle) {
            box(762,py+30,68,32,*toggle?(chosen?selected_ink:selection):window,Layer::button,chosen?selected_ink:muted,2,16);
            box(*toggle?804:768,py+36,20,20,ink,Layer::text,rule,0,10);
        } else icon(808, py + (row_height - 32) / 2, enabled ? (dropdown?"down":"next") : "info", color, 32);
        if (editing) text(690, py + 14, 104, 48, "＋ / －", ink, 29.333, true, true);
    }

    void button(const std::string& label, std::function<void()> fn, double px = 36,
        double py = 1018, double w = 650, const std::string& glyph = "",
        bool enabled = true) {
        const int index = bind(std::move(fn), enabled);
        const bool chosen = selected(index);
        const Color color = chosen ? selected_ink : enabled ? ink : muted;
        const auto begin = app.nodes.size();
        box(px, py, w, button_height, highlight(chosen,window), Layer::button,
            chosen ? selection : rule, 2);
        if (!glyph.empty()) icon(px + 20, py + 30, glyph, color);
        const double inset = glyph.empty() ? 24 : 84;
        text(px + inset, py, w - 2 * inset, button_height, label, color,
            29.333, false, false, true, true);
        buttons.emplace_back(begin, app.nodes.size());
    }

    void center_buttons() {
        // Sidebar and pagination controls belong to their own content columns.
        if (app.page == "HOME" || app.page == "REPORT" ||
            app.page == "DISPLAY" || app.page == "LAYER_EDIT" || app.page == "SETTINGS" ||
            buttons.empty() || buttons.size() > 2) return;
        const auto& first = app.nodes[buttons.front().first];
        double left = first.x, right = first.x + first.w;
        for (const auto& [begin, end] : buttons) {
            (void)end;
            const auto& box = app.nodes[begin];
            if (box.y != first.y) return;
            left = std::min(left, box.x);
            right = std::max(right, box.x + box.w);
        }
        const double shift = sheet_x + 720 - (left + right) / 2;
        for (const auto& [begin, end] : buttons)
            for (auto i = begin; i < end; ++i) app.nodes[i].x += shift;
    }

    void info(const std::vector<std::pair<std::string, std::string>>& items) {
        box(896, 264, 508, items.size() * 106 + 44);
        for (std::size_t i = 0; i < items.size(); ++i) {
            text(920, 284 + i * 106, 460, 40, items[i].first, muted, 25);
            text(920, 328 + i * 106, 460, 82, items[i].second,ink,26);
        }
    }

    void message(const std::string& heading, const std::string& body,
        const std::string& glyph = "info", double py = 332, double h = 306) {
        box(36, py, 1368, h);
        icon(64, py + 28, glyph, glyph == "ok" ? positive :
            glyph == "error" ? negative : glyph == "warning" ? neutral : ink, 52);
        text(140, py + 24, 1214, 64, heading, ink, 36, true);
        text(140, py + 106, 1200, h - 126, body, muted);
    }

    void content(const std::vector<std::string>& lines, double gap = 92, double py = 332) {
        box(36, py, 1368, lines.size() * gap + 32);
        for (std::size_t i = 0; i < lines.size(); ++i)
            text(64, py + 18 + i * gap, 1312, gap - 8, lines[i], i ? muted : ink);
    }

    void footer(const std::string& note = "") {
        box(36, 1168, 1368, 2, rule, Layer::background, rule, 0, 0);
        const bool edit = app.editing >= 0;
        const bool reverse=app.settings().reverse&&!edit, en=app.settings().locale=="en";
        constexpr double left = (1440 - 914) / 2.;
        icon(left, 1200, reverse?"down":"up", ink, 32);
        text(left + 42, 1194, 244, 42, edit ? msg::vol_increase : reverse?(en?"Vol + Next":msg::vol_next_2):msg::vol_previous,
            ink, 25, false, false, false, true);
        icon(left + 306, 1200, reverse?"up":"down", ink, 32);
        text(left + 348, 1194, 244, 42, edit ? msg::vol_decrease : reverse?(en?"Vol − Previous":msg::vol_previous_2):msg::vol_next,
            ink, 25, false, false, false, true);
        icon(left + 612, 1200, "power", ink, 30);
        text(left + 654, 1194, 260, 42, edit ? msg::power_done : msg::power_select,
            ink, 25, false, false, false, true);
        if (!note.empty())
            text(263, 1244, 914, 28, note, muted, 20, false, false, true, true);
    }

    void progress(double px, double py, double w, bool compact = false, bool awaiting_install = false) {
        const Task empty;
        const auto& task = awaiting_install ? empty : app.backend.task;
        const auto stages = stages_of(task);
        const double gap = 16;
        const double segment = (w - gap * (stages.size() - 1)) / stages.size();
        const double h = compact ? 6 : 12;
        const double track_y = py + (compact ? 30 : 44);
        text(px, py, w, compact ? 28 : 40, stage_summary(task,app.settings().locale=="en"), muted, compact ? 22 : 25);
        for (std::size_t i = 0; i < stages.size(); ++i) {
            const auto& stage = stages[i];
            const double sx = px + i * (segment + gap);
            box(sx, track_y, segment, h, rule, Layer::button, rule, 0, 5);
            const bool complete = stage.state == "done";
            const bool active = stage.state == "running" || stage.state == "waiting";
            const bool failed = stage.state == "error";
            const bool fraction = stage.fraction && std::isfinite(*stage.fraction) &&
                *stage.fraction >= 0 && *stage.fraction <= 1;
            if (complete || ((active || failed) && fraction))
                box(sx, track_y, segment * (complete ? 1 : *stage.fraction), h,
                    failed ? negative : selection, Layer::button, rule, 0, 5);
            else if (active || failed)
                box(sx, track_y, segment, h, failed ? negative :
                    Color{selection.r, selection.g, selection.b, 0.33},
                    Layer::button, rule, 0, 5, active ? Kind::stripes : Kind::box);
            const auto* words=progress_ui::progress_words(progress_ui::stage_words,stage.id);
            const bool en=app.settings().locale=="en";
            std::string_view label=words?words->short_label:msg::step;
            if (stage.state == "skipped") label = msg::skipped;
            text(sx, track_y + h + (compact ? 6 : 8), segment, compact ? 28 : 40,
                std::to_string(i + 1) + " " + std::string(localized(label,en)),
                failed ? negative : stage.state == "pending" ? muted : ink, compact ? 18 : 25);
        }
    }

    void status() {
        const double old_x = x, old_y = y;
        x = sheet_x; y = sheet_y;
        constexpr double w = 1440;
        const auto& telemetry = app.backend.telemetry;
        box(0, 0, w, 48, titlebar);
        box(0, 47, w, 1, rule, Layer::background, rule, 0, 0);
        // Match actual visible ink, rather than em size or icon container size.
        const auto status_text = [&](double px, double width, const std::string& value,
                                     Color color = ink, bool center = false, bool right = false,
                                     double height = 24) {
            text(px, 0, width, 48, value, color, 25, false, right, center, true);
            app.nodes.back().ink_height = height;
            app.nodes.back().cjk_bold = true;
        };
        const auto status_icon = [&](double px, const std::string& name, Color color,
                                     double width = 32) {
            icon(px, 8, name, color, 32);
            auto& node = app.nodes.back();
            node.w = width;
            node.ink_height = 28;
        };
        const auto rate = [&](double px, const std::optional<double>& value, const std::string& glyph) {
            const auto begin = app.nodes.size();
            const auto formatted = rate_text(value,app.settings().units);
            const auto at = formatted.find_first_not_of("0123456789.- ");
            const std::string digits = trimmed(formatted.substr(0, at));
            const std::string unit = at == std::string::npos ? "" : formatted.substr(at);
            status_icon(px, glyph, ink);
            status_text(px + 32, 60, digits, ink, false, true);
            status_text(px + 96, 72, unit, ink, false, false, 20);
            for (auto index = begin; index < app.nodes.size(); ++index)
                app.nodes[index].y += 56;
        };
        box(16, 0, 144, 47, titlebar, Layer::background, rule, 0, 0);
        const auto& state = telemetry.ncm_state;
        const Color network_color = state == "connected" ? ink :
            state == "connecting" || state == "no-address" || state == "suspended" ? neutral : muted;
        const auto flags=app.settings().visible;
        double cursor=16;
        for (const auto& [bit,name,color]:std::array<std::tuple<int,std::string,Color>,4>{{
                {1,"ncm:"+state,network_color},{2,"bluetooth-off",muted},
                {4,"wifi-off",muted},{8,"ethernet-off",muted}}}) {
            if (flags&bit) {
                const bool wide=bit==4 || (bit==1 && state=="connecting");
                status_icon(wide?cursor-2:cursor,name,color,wide?36:32);
                cursor+=36;
            }
        }
        const auto clock=clock_label(telemetry,app.settings());
        if (!clock.empty()) status_text(452,536,clock,telemetry.time_valid?ink:muted,true);
        const bool capacity_known = telemetry.capacity &&
            *telemetry.capacity >= 0 && *telemetry.capacity <= 100;
        const auto battery = "battery:" +
            (capacity_known ? std::to_string(*telemetry.capacity) : std::string("unknown")) +
            ":" + (telemetry.battery_status.empty() ? std::string("Unknown") : telemetry.battery_status);
        if(flags&16) {
        status_icon(w - 176, battery, !capacity_known ? muted :
            *telemetry.capacity <= 15 ? negative : ink, 44);
        app.nodes.back().ink_height = 26;
        status_text(w - 112, 88, capacity_known ?
            std::to_string(*telemetry.capacity) + "%" : msg::unknown, ink, false, true);
        app.nodes.back().previous_icon_gap = 8;
        }
        const bool power_known = telemetry.net_watts && std::isfinite(*telemetry.net_watts);
        const double watts = power_known ? *telemetry.net_watts : 0;
        if(flags&32)status_text((flags&16)?w-408:w-236, 212, power_known ?
            (watts > 0 ? "+" : "") + decimal(watts) + " W" : "n/a W",
            !power_known || watts == 0 ? muted : watts < 0 ? negative : positive, false, true);
        rate(w - 364, telemetry.upload_bps, "upload");
        rate(w - 192, telemetry.download_bps, "download");
        x = old_x; y = old_y;
    }
};

void clamp_log(App& app, const LogDocument& document) {
    app.log_top=std::clamp(app.log_top,0,document.max_top());
    app.log_cursor=std::clamp(app.log_cursor,0,std::max(0,document.row_count-1));
    if(!app.log_back)
        app.log_top=std::clamp(app.log_top,std::max(0,app.log_cursor-log_view_lines+1),
            std::min(app.log_cursor,document.max_top()));
    if(!document.row_count)app.log_back=true;
}

// Both viewers place the scroll indicator in the measured optical space,
// independently of the viewport's horizontal disparity.
struct ReaderRows {
    std::int64_t visual;
    int first,limit;
    double scroll_position() const { return static_cast<double>(visual)/log_line_px; }
};

ReaderRows visible_reader_rows(const App& app,int total,const ReaderViewport& viewport) {
    const auto px=app.reader_visual_top_px();
    const auto origin=px-viewport.top;
    const auto first=std::max<std::int64_t>(0,origin)/log_line_px;
    const auto limit=std::min<std::int64_t>(total,(origin+eye_size+log_line_px-1)/log_line_px);
    return {px,static_cast<int>(first),static_cast<int>(limit)};
}

void context_marker(Scene& scene,const ReaderViewport& viewport) {
    scene.box(0,viewport.top-sheet_y,0,viewport.bottom()-viewport.top,window,
              Layer::background,rule,0,0,Kind::reader_context);
}

void reader_row_nodes(Scene& scene,std::size_t first,int id,const ReaderViewport& viewport) {
    // Keep the geometry for full-height context, and give the opaque body
    // pass an independent clip so the original reading edges stay fixed.
    for(auto i=first;i<scene.app.nodes.size();++i){
        auto& node=scene.app.nodes[i];
        node.clip_y=viewport.top;node.clip_h=viewport.bottom()-viewport.top;
        node.reader_row=id;
        node.reader_context=true;
    }
}

void draw_scroll_arc(App& app, int y, int height, double offset, int visible, int total) {
    if(total<=visible)return;
    constexpr double center=eye_size/2.,radius=log_scroll_radius;
    const double begin=std::asin((y+log_scroll_inset-center)/radius);
    const double end=std::asin((y+height-log_scroll_inset-center)/radius);
    const auto arc=[&](double from,double to,double stroke,Color color){
        Node node;
        node.kind=Kind::arc;node.layer=Layer::background;
        node.x=node.y=center-radius;
        node.w=node.h=2*radius;
        node.arc_begin=from;node.arc_end=to;
        node.border_width=stroke;node.color=color;
        app.nodes.push_back(std::move(node));
    };
    const double span=end-begin;
    const double thumb=std::clamp(span*visible/total,24./radius,span);
    const double travel=(span-thumb)*std::clamp(offset,0.,static_cast<double>(total-visible))/(total-visible);
    arc(begin,end,4,rule);
    arc(begin+travel,begin+travel+thumb,6,muted);
}

void draw_report(Scene& scene) {
    auto& app=scene.app;
    auto& doc=app.report;
    const auto selected=std::find_if(reports.begin(),reports.end(),
        [&](const Report& r){return app.argument==r.id;});
    const bool valid=selected!=reports.end();
    constexpr std::string_view root="/run/overpressure/report/";
    constexpr Color terminal_bg{35/255.,38/255.,39/255.};
    constexpr Color terminal_header{49/255.,54/255.,59/255.};
    constexpr Color terminal_ink{252/255.,252/255.,252/255.};
    constexpr Color terminal_prompt{61/255.,174/255.,233/255.};
    const int w=app.log_width.viewport,x=app.log_width.x(),tx=x+108,text_w=app.log_width.text();
    if(!doc.loaded || doc.id!=app.argument){
        doc={};
        doc.id=app.argument;
        doc.loaded=true;
        if(valid){
            doc.output=app.backend.read(std::string(root)+selected->file,text_limit);
            if(selected->extra){
                const std::string path=selected->extra;
                // A leading slash is used only by the fixed /proc/meminfo entry.
                auto extra=app.backend.read(path.starts_with('/')?path:std::string(root)+path,
                                            path.starts_with('/')?65536:text_limit);
                if(!doc.output.empty()&&!doc.output.ends_with('\n')&&!extra.empty()) doc.output+='\n';
                doc.output+=extra;
            }
            if(doc.output.ends_with('\n')) doc.output.pop_back();
        }
    }
    if(doc.width!=text_w){
        const auto pos=static_cast<std::size_t>(doc.top_line);
        const auto offset=pos<doc.lines.size()?doc.lines[pos].offset:0;
        doc.lines=doc.output.empty()?std::vector<LogLine>{}:wrap_log(doc.output,text_w-2,app.measure_log_glyph);
        const auto end=std::upper_bound(doc.lines.begin(),doc.lines.end(),offset,
            [](std::size_t byte,const LogLine& line){return byte<line.offset;});
        doc.top_line=end==doc.lines.begin()?0:static_cast<int>(end-doc.lines.begin()-1);
        doc.width=text_w;
    }
    doc.top_line=std::clamp(doc.top_line,0,doc.max_top());

    scene.page_background();
    const auto back=scene.bind([&app]{app.back();});
    const bool back_focused=scene.selected(back);
    scene.box(36,56,276,62,scene.highlight(back_focused,window),Layer::button,
              back_focused?selection:rule,2);
    scene.icon(52,67,"back",back_focused?selected_ink:ink,34);
    scene.text(36,56,276,62,msg::back,back_focused?selected_ink:ink,25,false,false,true,true);
    scene.text(346,56,680,42,valid?selected->name:msg::hardware_report,ink,29.333,true);
    scene.text(346,96,680,29,valid?msg::read_only_run_overpressure_report:msg::read_only,muted,20);

    constexpr auto viewport=reader_viewport(true);
    constexpr int command_y=log_viewport_top-static_cast<int>(sheet_y);
    scene.box(x,viewport.top-sheet_y,w,viewport.bottom()-viewport.top,terminal_bg,Layer::background,rule,0,0);
    app.nodes.back().reader_backdrop=true;
    scene.box(x,command_y,w,report_command_height,terminal_header,Layer::background,rule,0,0);
    std::string query=valid?"$ cat "+std::string(selected->file):msg::report_source_not_found;
    if(valid&&selected->extra) query+=" "+std::string(selected->extra);
    scene.text(tx,command_y+10,text_w,log_line_px,query,terminal_prompt,log_font_px);
    app.nodes.back().prewrapped=valid;
    scene.box(x,command_y+report_command_height-1,w,1,rule,Layer::background,rule,0,0);

    constexpr int y=viewport.top-static_cast<int>(sheet_y);
    const int count=static_cast<int>(doc.lines.size());
    const auto rows=visible_reader_rows(app,count,viewport);
    context_marker(scene,viewport);
    if(doc.lines.empty())
        scene.text(tx,y,text_w,log_line_px,
                   msg::no_readable_report_go_back_to_choose_another_category,muted,log_font_px);
    for(int i=rows.first;i<rows.limit;++i){
        const auto& line=doc.lines[i].text;
        const auto at=y+std::int64_t{i}*log_line_px-rows.visual;
        const auto start=app.nodes.size();
        scene.text(tx,at,text_w,log_line_px,line,terminal_ink,log_font_px);
        auto& node=app.nodes.back();
        node.value=line;node.prewrapped=true;
        reader_row_nodes(scene,start,i,viewport);
    }

    draw_scroll_arc(app,log_viewport_top+report_command_height,log_height-report_command_height,
                    rows.scroll_position(),report_view_lines,count);
    if(doc.max_top()>0)scene.text(36,1189,760,40,msg::volume_scroll_power_back,muted,25);
}

void draw_logs(Scene& scene) {
    auto& app = scene.app;
    const auto& document = log_document(app);
    clamp_log(app, document);
    const auto& task = app.backend.task;
    const auto progress = log_progress::presentation(app, installation_task(task));
    const auto w=app.log_width.viewport,x=app.log_width.x(),text_x=x+108,text_w=app.log_width.text();
    scene.page_background();
    const int index = scene.bind([&app] { app.back(); });
    const bool back = app.log_back && scene.selected(index);
    const bool live=app.log_source=="install-live";
    if(app.backend.manual_preview){
        scene.box(36,56,276,62,window,Layer::button,rule,2);
        scene.text(36,57,276,30,msg::power_play_pause,ink,21,false,false,true,true);
        scene.text(36,87,276,29,msg::volume_prev_next,muted,18,false,false,true,true);
    }else{
        scene.box(36, 56, 276, 62, back ? selection : window, Layer::button,back ? selection : rule, 2);
        scene.icon(52, 67, "back", back ? selected_ink : ink, 34);
        scene.text(36, 56, 276, 62, live?msg::back_to_home:msg::back, back ? selected_ink : ink, 25,false, false, true, true);
    }
    if(live){
        const std::string title=app.backend.install_preview?msg::installation_preview_live_log:msg::steamos_installation_live_log;
        scene.text(322,56,680,36,title,ink,29,true);
        scene.text(322,92,680,29,app.follow_install_log?msg::following_new_entries:msg::browsing_log_history,muted,20);
    }
    scene.bind([&app] { app.back(); });
    constexpr auto viewport=reader_viewport(false);
    scene.box(x,128,w,viewport.bottom()-viewport.top,view,Layer::background,rule,0,0);
    app.nodes.back().reader_backdrop=true;
    context_marker(scene,viewport);
    if (document.entries.empty()) {
        scene.text(text_x, 156, text_w, 168, progress == log_progress::Presentation::awaiting_install
            ? msg::continue_installation_from_the_host_using_the_existing_workflow : msg::no_readable_log_entries,
            muted, log_font_px);
    }
    const auto rows=visible_reader_rows(app,document.row_count,viewport);
    if(rows.first<rows.limit){
        auto entry=entry_at(document,rows.first);
        for(int row=rows.first;row<rows.limit;++row){
            int line=row-entry->first_row;
            if(line==static_cast<int>(entry->lines.size())){++entry;line=0;}
            const int y=128+std::int64_t{row}*log_line_px-rows.visual;
            const bool chosen=!app.log_back&&row==app.log_cursor&&!app.dialog;
            const auto index=entry-document.entries.begin();
            const auto begin=app.nodes.size();
            scene.box(x+8,y,w-26,log_line_px,chosen?selection:index%2?alternate:view,
                      Layer::button,rule,0,chosen?6:0);
            if(!line)scene.text(x+32,y,64,log_line_px,std::to_string(index+1),
                                chosen?selected_ink:muted,log_font_px,false,true);
            const auto& value=entry->lines[line].text;
            scene.text(text_x,y,text_w,log_line_px,value,chosen?selected_ink:ink,log_font_px);
            app.nodes.back().value=value;
            app.nodes.back().prewrapped=true;
            if(chosen)scene.icon(x+12,y+10,"next",selected_ink,22);
            reader_row_nodes(scene,begin,row,viewport);
        }
    }
    draw_scroll_arc(app,log_viewport_top,log_height,rows.scroll_position(),log_view_lines,document.row_count);
    if (progress == log_progress::Presentation::compact) {
        const auto start = app.nodes.size();
        scene.progress(36, 1172, 1368, true);
        if (task_failed(task) || task_succeeded(task)) {
            app.nodes[start].value = localized(task_status(task.status), app.settings().locale == "en");
            const auto first = app.nodes.begin() + start;
            app.nodes.erase(std::remove_if(first, app.nodes.end(), [](const Node& node) {
                return node.kind == Kind::stripes;
            }), app.nodes.end());
        } else if (!log_progress::compact_activity(task)) {
            for (auto i = start; i < app.nodes.size(); ++i)
                if (app.nodes[i].kind == Kind::stripes) app.nodes[i].value = "still";
        }
    } else if (progress == log_progress::Presentation::awaiting_install) {
        scene.text(36, 1172, 1368, 28, msg::installation_has_not_started_waiting_for_the_host, muted, 22);
    } else if (progress == log_progress::Presentation::summary) {
        scene.text(36, 1172, 1368, 28, task_status(task.status), muted, 22);
    }
    app.focus = app.log_back ? 0 : 1;
}

} // namespace

namespace progress_ui {
namespace {
constexpr Color aqua{145/255.,201/255.,236/255.}, amber{244/255.,186/255.,128/255.};
constexpr Color failure{255/255.,182/255.,192/255.}, deck_rule{77/255.,101/255.,111/255.};
constexpr Color child_track{85/255.,94/255.,105/255.}, child_ink{185/255.,195/255.,203/255.};

bool terminal(std::string_view state) {return state=="done"||state=="skipped";}
bool current(std::string_view state) {return state=="running"||state=="waiting"||state=="error";}
bool valid(std::optional<double> fraction) {return fraction&&*fraction>=0&&*fraction<=1;}
bool trusted(const Substep& sub) {return sub.measured&&valid(sub.fraction);}
bool in_route(const Substep& sub) {
    return sub.weight>0||current(sub.state)||sub.state=="done";
}

std::string label(const ProgressWords* words,std::string_view name,bool en,bool full) {
    if(words)return std::string(localized(full?words->full:words->short_label,en));
    return install_log::message(name.empty()?msg::step:name,en);
}

std::string substep_detail(const Stage& stage,const Substep& step,bool en) {
    if(step.state=="error"&&!step.name.empty())return std::string(localized(step.name,en));
    if((stage.id=="boot"||stage.id=="super")&&(step.id=="write"||step.id=="read"||step.id=="inspect"||
       step.id=="existing-hash"))
        return formatted(step.id=="write"?msg::writing_arg0:step.id=="inspect"?msg::checking_the_arg0_image:
                         step.id=="existing-hash"?msg::verifying_existing_arg0:msg::reading_back_arg0_for_sha_256,en,{stage.id});
    return label(progress_words(child_words,step.id),step.name,en,true);
}

std::string count_label(const Substep& sub,bool en) {
    if(!trusted(sub)||sub.total<=0||sub.done<0||sub.done>sub.total)return {};
    if(sub.unit=="percent")return formatted(msg::this_pass_arg0,en,{std::to_string(
        static_cast<int>(std::floor(100.*sub.done/sub.total)))});
    if(sub.unit=="bytes") {
        const double divisor=sub.total>=1048576?1048576.:sub.total>=1024?1024.:1.;
        const auto amount=[&](std::int64_t value) {
            return divisor==1?std::to_string(value):decimal(value/divisor,sub.total/divisor<100?2:1);
        };
        return amount(sub.done)+" / "+amount(sub.total)+(divisor==1048576?" MiB":divisor==1024?" KiB":" B");
    }
    const auto unit=sub.unit=="packages"?localized(msg::packages,en):sub.unit=="files"?localized(msg::files,en):std::string_view{};
    if(unit.empty())return {};
    return std::to_string(sub.done)+" / "+std::to_string(sub.total)+" "+std::string(unit);
}

struct Fitted {
    std::string value;
    double size,width;
};

Fitted fit(const Measure& measure,std::string_view value,double room,double size,bool bold,double minimum=12) {
    value=value.substr(0,value.find_first_of("\r\n\t"));
    if(room<=0||value.empty())return {{},size,0};
    double width=measure(value,size,bold);
    while(width>room&&size>minimum*scale) {
        size=std::max(minimum*scale,std::min(size-.25*scale,size*room/width));
        width=measure(value,size,bold);
    }
    if(width<=room)return {std::string(value),size,width};
    const std::string ellipsis="…";
    width=measure(ellipsis,size,bold);
    if(width>room)return {{},size,0};
    std::string best=ellipsis;
    // Find the longest fitting UTF-8 prefix without splitting a glyph.
    std::size_t lo=0,hi=value.size();
    while(lo+1<hi) {
        std::size_t end=(lo+hi)/2;
        while(end>lo&&(static_cast<unsigned char>(value[end])&0xc0)==0x80)--end;
        if(end==lo) {
            end=(lo+hi)/2+1;
            while(end<hi&&(static_cast<unsigned char>(value[end])&0xc0)==0x80)++end;
            if(end==hi)break;
        }
        auto text=std::string(value.substr(0,end))+ellipsis;
        if(const double w=measure(text,size,bold);w<=room){lo=end;width=w;best=std::move(text);}
        else hi=end;
    }
    return {std::move(best),size,width};
}

struct DeckDraw {
    const Measure& measure;
    std::vector<Node>& nodes;
    void box(double x,double y,double w,double h,Color color,double radius=0,
             std::string_view pattern={},double stroke=0) {
        if(w<=0||h<=0)return;
        Node n;
        n.kind=pattern.empty()?Kind::box:Kind::stripes;
        n.layer=Layer::button;n.x=x;n.y=y;n.w=w;n.h=h;n.color=color;
        n.radius=radius;n.value=pattern;n.border=amber;n.border_width=stroke;
        nodes.push_back(std::move(n));
    }
    void text(double x,double y,double w,double h,const Fitted& text,Color color,bool bold=false,bool center=false) {
        if(text.value.empty()||w<=0)return;
        Node n;
        // Use the rail's disparity for all labels, so narrow gaps stay fixed in both eyes.
        n.kind=Kind::text;n.layer=Layer::button;n.x=x;n.y=y;n.w=w;n.h=h;n.value=text.value;
        n.size=text.size;n.color=color;n.bold=bold;n.center=center;n.middle=true;
        nodes.push_back(std::move(n));
    }
};

void child_deck(DeckDraw& draw,const Stage& stage,std::span<const Substep* const> route,const Substep* active,double parent,
                bool en,bool running,Animation& motion,double now,bool animate) {
    std::vector<unsigned char> widths;
    std::vector<std::string> ids;
    std::size_t done=0;
    for(const auto* sub:route) {
        widths.push_back(sub->measured);
        ids.push_back(sub->id);
        done+=terminal(sub->state);
    }
    if(ids!=motion.child_ids){motion.child_ids=std::move(ids);motion.child_fills.assign(widths.size(),{});animate=false;}
    const auto layout=child_layout(widths,parent);
    if(layout.rows.empty())return;
    const auto& first=layout.rows.front();
    const auto& last=layout.rows.back();
    const bool anchored=widths.size()<=anchor_steps;
    const double heading_width=anchored?std::min(chord(child_top)-2*std::abs(layout.x-center),
        std::max(280*scale,first.w)):chord(child_top);
    const auto words=progress_words(stage_words,stage.id);
    const auto title=formatted(msg::arg0_substeps,en,{label(words,stage.name,en,!anchored)});
    auto summary=formatted(msg::arg0_arg1_done,en,{std::to_string(done),std::to_string(widths.size())});
    if(last.begin+last.count<widths.size())summary+=" · +"+std::to_string(widths.size()-last.begin-last.count);
    const double left=layout.x-heading_width/2,right=layout.x+heading_width/2;
    const auto count=fit(draw.measure,summary,heading_width*.48,19*scale,false);
    const auto heading=fit(draw.measure,title,heading_width-count.width-16*scale,19*scale,false);
    draw.box(left,child_top,heading_width,2*scale,deck_rule);
    draw.text(left+3*scale,child_top+2*scale,heading.width+1,19*scale,heading,ink);
    draw.text(right-count.width-4*scale,child_top+2*scale,count.width+1,19*scale,count,muted);

    for(const auto& row:layout.rows) {
        double x=layout.x-row.w/2;
        for(std::size_t i=row.begin;i<row.begin+row.count;++i) {
            const auto& sub=*route[i];
            const double w=widths[i]?meter_width:row.status;
            const bool selected=&sub==active,runs=selected&&running&&sub.state=="running";
            const bool wait=sub.state=="waiting",error=sub.state=="error",skip=sub.state=="skipped";
            const bool activity=runs&&!trusted(sub);
            const auto number=(i<9?"0":"")+std::to_string(i+1);
            const auto ordinal=fit(draw.measure,number,w,15*scale,false);
            const double label_x=x+ordinal.width+5*scale;
            const double label_w=w-ordinal.width-5*scale;
            const auto words=progress_words(child_words,sub.id);
            const auto name=fit(draw.measure,label(words,sub.name,en,false),label_w,19*scale,false);
            const auto shown=motion.child_fills[i].update(sub.state=="done"?1:
                trusted(sub)&&current(sub.state)?*sub.fraction:0,now,animate);
            motion.tween|=motion.child_fills[i].moving(now);
            motion.activity|=activity;
            if(runs)draw.box(x,row.y-24*scale,w,24*scale,{36/255.,55/255.,69/255.},4*scale);
            draw.text(x,row.y-24*scale,ordinal.width+1,22*scale,ordinal,aqua);
            draw.text(label_x,row.y-24*scale,label_w,22*scale,name,error?failure:wait?amber:runs?ink:child_ink,false,true);
            const double h=(runs?7:5)*scale,y=row.y-h/2;
            draw.box(x,y,w,h,skip?muted:child_track,3*scale,skip?"progress-skip-child":"");
            if(activity)draw.box(x,y,w,h,selection,3*scale,"progress-child");
            else if(!skip&&shown>0)draw.box(x,y,w*shown,h,error?negative:wait?amber:
                sub.state=="done"?Color{100/255.,160/255.,190/255.}:selection,3*scale);
            else if((wait||error)&&!trusted(sub))draw.box(x,y,w,h,wait?amber:negative,3*scale);
            if(wait||error) {
                const auto state=fit(draw.measure,wait?"Ⅱ":"!",14*scale,15*scale,true);
                draw.text(x+w-16*scale,row.y-8*scale,14*scale,16*scale,state,wait?amber:failure,true,true);
            }
            x+=w+child_gap;
        }
    }
}
} // namespace

bool has_log_deck(const App& app) {
    return log_progress::presentation(app, installation_task(app.backend.task)) ==
        log_progress::Presentation::deck;
}

void draw_log_deck(const App& app,Animation& motion,const Measure& measure,std::vector<Node>& out) {
    if(!has_log_deck(app)){motion={};return;}
    const auto& task=app.backend.task;
    const auto& stages=task.stages;
    const double now=app.animation_phase;
    const bool en=app.settings().locale=="en";
    const std::string key=task.kind+":"+task.id;
    std::vector<std::string> ids;
    for(const auto& stage:stages)ids.push_back(stage.id.empty()?stage.name:stage.id);
    const bool reset=motion.task!=key||motion.stage_ids!=ids;
    if(reset) {
        motion={};motion.task=key;motion.stage_ids=std::move(ids);
        motion.stage_fills.resize(stages.size());
    }
    const bool animate=now>0&&!app.reduced_motion&&!reset;
    motion.tween=motion.activity=false;
    const bool failed=task_failed(task),complete=task_succeeded(task);
    const bool handoff=task.status=="waiting"&&std::all_of(stages.begin(),stages.end(),
        [](const Stage& stage){return terminal(stage.state);});
    std::size_t active=stages.size();
    if(!complete&&!handoff) {
        if(task.active>=0&&static_cast<std::size_t>(task.active)<stages.size()&&current(stages[task.active].state))
            active=static_cast<std::size_t>(task.active);
        else {
            const auto it=std::find_if(stages.begin(),stages.end(),[](const Stage& s){return current(s.state);});
            active=static_cast<std::size_t>(it-stages.begin());
        }
    }
    const Stage* selected=active<stages.size()?&stages[active]:nullptr;
    const Substep* child=nullptr;
    std::vector<const Substep*> route;
    if(selected) {
        const auto& steps=selected->substeps;
        auto found=std::find_if(steps.begin(),steps.end(),[&](const Substep& s){return s.state==selected->state;});
        if(found==steps.end())found=std::find_if(steps.begin(),steps.end(),[](const Substep& s){return current(s.state);});
        if(found==steps.end()&&selected->state=="running")
            found=std::find_if(steps.begin(),steps.end(),[&](const Substep& s) {
                return s.state=="skipped"&&!s.name.empty()&&s.name==task.detail;
            });
        if(found!=steps.end())child=&*found;
        for(const auto& step:steps)if(in_route(step)||&step==child)route.push_back(&step);
    }
    const bool expanded=route.size()>1;
    const bool running=selected&&selected->state=="running"&&!failed;
    const bool waiting=selected&&(selected->state=="waiting"||(child&&child->state=="waiting"));
    const bool error=failed||(selected&&selected->state=="error")||(child&&child->state=="error");
    const auto target=main_rail(stages.size(),expanded?active:stages.size());
    const auto rail=motion.rail(target,now,animate);
    motion.tween=motion.rail_moving(now);
    DeckDraw draw{measure,out};
    draw.box(main_left,deck_top,main_width,(child_top-deck_top),window);
    out.back().value="progress-deck";

    const auto translate=[&](std::string_view text){return std::string(localized(text,en));};
    std::string title,phase,measurement;
    if(selected) {
        title=label(progress_words(stage_words,selected->id),selected->name,en,true);
        if(child) {
            phase=substep_detail(*selected,*child,en);
            measurement=count_label(*child,en);
            if(measurement.empty()&&child->state=="skipped")measurement=translate(msg::skipped_2);
            if(measurement.empty()&&running&&child->state=="running")
                measurement=translate(trusted(*child)?msg::in_progress:msg::total_unknown);
        } else {
            phase=task.detail.empty()?translate(task_status(selected->state)):translate(task.detail);
            if(running&&selected->substeps.empty())measurement=valid(selected->fraction)?decimal(std::floor(*selected->fraction*100),0)+"%":
                translate(msg::total_unknown);
        }
        if(error||waiting) {
            const auto status=translate(error?msg::failed:msg::waiting);
            measurement=measurement.empty()?status:status+" · "+measurement;
        }
        if(failed&&!task.error.empty())phase=install_stage_detail(task.error,en);
    } else if(handoff)title=translate(msg::install_steps_complete_first_boot_not_yet_verified);
    else if(complete){title=translate(msg::installation_complete);phase=install_stage_detail(task.detail,en);}
    else if(failed){title=translate(task_status(task.status));phase=install_stage_detail(task.error,en);}
    else title=translate(stage_summary(task,en));
    const auto ordinal=fit(measure,selected?std::to_string(active+1)+" / "+std::to_string(stages.size()):"",
        main_width,24*scale,false);
    const auto amount=fit(measure,measurement,main_width*.36,23*scale,false,17);
    const double available=main_width-(amount.width>0?amount.width+20*scale:0);
    const auto heading=fit(measure,title,phase.empty()?available-ordinal.width-14*scale:
        std::min(available*.50,available-ordinal.width-14*scale),29*scale,true,20);
    const double heading_left=main_left+(ordinal.width>0?ordinal.width+14*scale:0);
    const double phase_left=heading_left+heading.width+14*scale;
    const auto detail=fit(measure,phase.empty()?"":"· "+phase,main_left+available-phase_left,26*scale,false,20);
    const double meta_y=deck_top+5*scale,meta_h=39*scale;
    draw.text(main_left,meta_y,ordinal.width+1,meta_h,ordinal,aqua);
    draw.text(heading_left,meta_y,heading.width+1,meta_h,heading,ink,true);
    draw.text(phase_left,meta_y,detail.width+1,meta_h,detail,error?failure:complete?positive:muted);
    draw.text(main_left+main_width-amount.width-1,meta_y,amount.width+1,meta_h,amount,error?failure:waiting?amber:muted);

    for(std::size_t i=0;i<stages.size();++i) {
        const auto& stage=stages[i];
        const auto [x,w]=rail[i];
        const bool is_active=i==active,focus=is_active&&expanded;
        const bool failed_stage=stage.state=="error"||(is_active&&error);
        const bool wait=!failed_stage&&(stage.state=="waiting"||(is_active&&waiting)),skip=stage.state=="skipped";
        const double h=(wait?9:focus?12:6)*scale,y=main_axis-h/2;
        const double value=stage.state=="done"?1:valid(stage.fraction)&&current(stage.state)?*stage.fraction:0;
        // Live reweighting or a retry may lower the target. Hold the displayed
        // fill until new evidence catches up; replay may explicitly rewind.
        const double fill=motion.stage_fills[i].update(value,now,animate,!app.backend.install_preview);
        motion.tween|=motion.stage_fills[i].moving(now);
        const Color clear{0,0,0,0};
        if(!wait)draw.box(x,y,w,h,skip?muted:rule,5*scale,skip?"progress-skip-main":"");
        if(!skip&&fill>0)draw.box(x,y,w*fill,h,failed_stage?negative:wait?amber:selection,5*scale);
        if(wait)draw.box(x+scale,y,w-2*scale,h,clear,5*scale,{},2*scale);
        double weight=0;
        for(const auto& sub:stage.substeps)weight+=sub.weight;
        if(is_active&&running&&!error&&!waiting) {
            const bool unknown=child?child->state=="running"&&!trusted(*child):
                stage.substeps.empty()&&!valid(stage.fraction);
            if(unknown) {
                const double start=std::max(fill,value);
                // Texture marks work in flight; neither time nor host bytes grow solid fill.
                const double share=child?(weight>0?child->weight/weight:0):1;
                const double visible=std::min(1-start,child?.84*share:share)*w;
                if(visible>0){draw.box(x+start*w,y,visible,h,selection,4*scale,"progress-main");motion.activity=true;}
            }
        }
        if(focus&&weight>0) {
            double offset=0, previous=0;
            for(const auto& sub:stage.substeps) {
                offset+=sub.weight;
                const double tick=w*offset/weight;
                if(tick-previous<2*scale||tick>w-2*scale)continue;
                draw.box(x+tick,y+2*scale,scale,h-4*scale,{226/255.,242/255.,1,.34});
                previous=tick;
            }
        }
        const auto words=progress_words(stage_words,stage.id);
        auto name=label(words,stage.name,en,false);
        const bool bold=focus||stage.state=="running";
        const auto number=fit(measure,std::to_string(i+1),w,12*scale,true);
        const double badge_w=std::max(15*scale,number.width+4*scale),label_room=w-badge_w-8*scale;
        if(words&&!words->compact.empty()&&measure(name,17*scale,bold)>label_room)
            name=localized(words->compact,en);
        const auto short_name=fit(measure,name,label_room,(focus?21:19)*scale,bold);
        const double left=x+(w-badge_w-6*scale-short_name.width)/2;
        const double label_y=main_axis+13*scale;
        draw.box(left,label_y+4.5*scale,badge_w,15*scale,focus?Color{41/255.,62/255.,81/255.}:
                 Color{52/255.,58/255.,67/255.},3*scale);
        draw.text(left,label_y,badge_w,24*scale,number,failed_stage?failure:wait?amber:focus||stage.state=="done"?aqua:muted,true,true);
        draw.text(left+badge_w+6*scale,label_y,short_name.width+1,24*scale,short_name,
                  failed_stage?failure:bold?ink:muted,bold);
        if(focus)draw.box(x+w/2-scale,main_axis+46*scale,2*scale,6*scale,deck_rule);
    }

    if(expanded) {
        const std::string deck=std::to_string(active)+":"+selected->id;
        const bool opening=motion.deck!=deck||motion.closing;
        if(opening){motion.deck=deck;motion.closing=false;motion.deck_at=animate?now:now-1;}
        motion.deck_nodes.clear();
        DeckDraw children{measure,motion.deck_nodes};
        child_deck(children,*selected,route,child,rail[active].x+rail[active].w/2,en,running,motion,now,animate&&!opening);
    } else if(!motion.deck_nodes.empty()&&!motion.closing) {
        motion.closing=true;motion.deck_at=now;
    }
    if(!animate||(motion.closing&&now-motion.deck_at>=.200)) {
        if(motion.closing) {
            motion.deck.clear();motion.deck_nodes.clear();motion.child_ids.clear();motion.closing=false;
        }
        else motion.deck_at=now-1;
    }
    if(motion.deck_nodes.empty())return;
    const double elapsed=now-motion.deck_at;
    const double opacity=motion.closing?1-std::pow(std::clamp(elapsed/.160,0.,1.),2):
        1-std::pow(1-std::clamp(elapsed/.200,0.,1.),2);
    const double shift=motion.closing?-7*scale*curve(elapsed/.200,deck_close_curve):
        -7*scale*(1-curve(elapsed/.270,deck_open_curve));
    motion.tween|=elapsed<(motion.closing?.200:.270);
    for(auto node:motion.deck_nodes) {
        node.y+=shift;node.color.a*=opacity;node.border.a*=opacity;
        if(motion.closing&&node.kind==Kind::stripes&&node.value=="progress-child")node.value="progress-child-still";
        out.push_back(std::move(node));
    }
}
} // namespace progress_ui

int LogWidth::text() const {
    // Leave room for the arc at its innermost point, including the maximum
    // text disparity. Keep a single left-aligned text width at every Y.
    constexpr int center=eye_size/2;
    constexpr int dy=std::max(center-(log_viewport_top+log_scroll_inset),
                             log_viewport_top+log_height-log_scroll_inset-center);
    constexpr int max_text_shift=(12+8)/2;
    const int arc_left=center+static_cast<int>(std::sqrt(log_scroll_radius*log_scroll_radius-dy*dy));
    const int text_left=360+x()+108;
    return std::min(viewport-144,arc_left-3-max_text_shift-12-text_left);
}

App::App(bool preview, bool defer_startup) {
    backend.install_preview=preview;
    if(defer_startup)page="BOOT";
    else finish_startup();
}

void App::finish_startup() {
    backend.preferences = backend.load_ui_preferences();
    const auto catalog = supported_locales();
    if (std::find(catalog.begin(), catalog.end(), backend.preferences.locale) == catalog.end() ||
        !backend.preferences.durable)
        backend.preferences.language_selected = false;
    draft = depth;
    first_install_entry=!backend.has_system_image();
    if (!backend.preferences.language_selected) {
        backend.preferences.locale="en";
        backend.preferences.durable=false;
        if(!first_install_entry)open_home();
        else open_install_log();
        return;
    }
    if(first_install_entry)open_install_log();
    else open_home();
}

void App::open_home(int initial) {
    cancel_reader_scroll();
    stack.clear();argument.clear();notice.clear();editing=-1;
    if(!backend.preferences.language_selected){
        page="LANGUAGE";focus=0;
        notice=preference_notice(backend.preferences.error,false);
        return;
    }
    page = "HOME";
    focus = initial;
}

void App::open_install_log() {
    cancel_reader_scroll();
    stack.clear();argument.clear();notice.clear();
    page="LOG_VIEW";log_source="install-live";
    log_back=follow_install_log=true;
    log_top=log_cursor=0;
    log_layout={};
    focus = 0;
}

bool App::observe_install_task() {
    const auto& task = backend.task;
    if (backend.install_preview || task.id.empty() || task.id == seen_install_task ||
        !task.locked || task.status != "running" || !installation_task(task))
        return false;
    seen_install_task = task.id;
    if (page == "LOG_VIEW" && log_source == "install-live") return false;
    if (language_save_initial && (language_save.valid() || uncommitted)) return false;
    if (dialog) back();
    dialog = false;
    choice.clear();
    held_power.clear();
    power_held = false;
    open_install_log();
    return true;
}

void App::navigate(const std::string& id, const std::string& arg, int initial) {
    if (dialog || !choice.empty()) return;
    if (!backend.preferences.language_selected && page == "LANGUAGE" && argument != "settings") return;
    cancel_reader_scroll();
    if(id=="HOME"){open_home(std::max(0,initial));return;}
    if (!known_page(id)) {
        notice = msg::this_page_is_unavailable_no_action_was_run;
        stack.push_back({page, argument, focus});
        page = "UNAVAILABLE";
        argument.clear();
        focus = 0;
        return;
    }
    if (id == "CONFIRM") {
        const auto operation = request(backend, arg, settings().locale=="en");
        if (!operation.reason.empty() || operation.action.empty()) {
            notice = operation.reason;
            navigate("UNAVAILABLE", arg, 0);
        } else {
            const bool en=settings().locale=="en";
            confirm(formatted(msg::confirm_action_arg0,en,{std::string(localized(operation.name,en))}),
                    operation.effect,operation.action,operation.argument);
        }
        return;
    }
    stack.push_back({page, argument, focus});
    if (page == "DISPLAY" || page == "LAYER_EDIT") {
        if (id != "LAYER_EDIT" && id != "DISPLAY") draft = depth;
    }
    if (id == "DISPLAY") draft = depth;
    editing = -1;
    page = id;
    argument = id == "LANGUAGE" ? "settings" : arg;
    focus = std::max(0, initial);
    if(report_detail()) report={};
    if (id == "LOG_VIEW") {
        const auto source = arg.empty() ? std::string("task") : arg;
        if (log_source != source) { log_cursor = 0; log_top = 0; }
        log_source = source;
        follow_install_log=source=="install-live";
        if(follow_install_log)log_layout={};
        log_back = true;
        focus = 0;
    }
}

void App::back() {
    cancel_reader_scroll();
    if (!choice.empty()) { choice.clear();return; }
    if (dialog) {
        if(deadline_confirmation && uncommitted && save_error.empty() && !language_save.valid() &&
           !same_settings(*uncommitted,backend.preferences)) uncommitted.reset();
        deadline_confirmation.reset();
        dialog = false;
        dialog_focus = 1;
        dialog_action.clear();
        dialog_argument.clear();
        return;
    }
    if (!backend.preferences.language_selected && page == "LANGUAGE" && argument != "settings"){
        if(first_install_entry&&!language_save.valid())open_install_log();
        return;
    }
    if (page == "LAYER_EDIT") {
        editing = -1;
        page = "DISPLAY";
        return;
    }
    if (page == "DISPLAY") { draft = depth; editing = -1; }
    if (page == "LOG_VIEW") log_back = true;
    notice.clear();
    if (stack.empty()) {
        open_home();
        return;
    }
    const auto saved = std::move(stack.back());
    stack.pop_back();
    page = saved.page;
    argument = saved.argument;
    focus = saved.focus;
    if (page == "LOG_VIEW") {
        if (log_source != argument) { log_cursor = 0; log_top = 0; }
        log_source = argument;
        follow_install_log=log_source=="install-live";
        log_back = true;
        focus = 0;
        if(follow_install_log){log_top=log_layout.max_top();log_cursor=std::max(0,log_layout.row_count-1);}
    }
}

void App::confirm(const std::string& title, const std::string& body,
    const std::string& action, const std::string& arg) {
    if (dialog) return;
    const std::string key = action == "bootmode" ? arg :
        action == "probe" ? "probe" : action + ":" + arg;
    const auto operation = request(backend, key, settings().locale=="en");
    if (!backend.preferences.language_selected || operation.action != action ||
        operation.argument != arg || !operation.reason.empty()) {
        notice = operation.reason.empty() ? msg::this_action_is_not_supported : operation.reason;
        navigate("UNAVAILABLE", key, 0);
        return;
    }
    cancel_reader_scroll();
    dialog_title = title;
    dialog_body = body;
    dialog_action = action;
    dialog_argument = arg;
    dialog_focus = 1;
    dialog = true;
}

namespace {
struct DeadlineState { int now=0,connect=300,idle=900,test=0,reachable=0,ack=0,contact=0; bool known=false; };
DeadlineState deadline_state(const Backend& b) {
    DeadlineState d;
    std::istringstream in(b.read("/run/overpressure/timeout-state",128));
    d.known=bool(in>>d.now>>d.connect>>d.idle>>d.test>>d.reachable>>d.ack>>d.contact);
    if(!d.known) d=DeadlineState{};
    return d;
}
int effective_limit(int value,int fallback) { return value==0?0:value>fallback?value:fallback; }
bool expired_change(const Backend& b,const Preferences& p) {
    const auto d=deadline_state(b);
    const bool connect=p.connect!=b.preferences.connect;
    const bool idle=p.idle!=b.preferences.idle;
    if(!d.known) return (connect&&p.connect!=0)||(idle&&p.idle!=0);
    const int a=effective_limit(p.connect,d.connect),c=effective_limit(p.idle,d.idle);
    // Policy state is sampled by the watchdog every two seconds.
    return (connect&&a>0&&d.now+2>=a&&(d.test?!d.ack:!d.reachable))||
        (idle&&d.test&&c>0&&d.now+2-d.contact>=c);
}
std::vector<std::pair<int,std::string>> options(const App& a,const std::string& key) {
    using V=std::vector<std::pair<int,std::string>>;
    if(key=="hour")return V{{24,msg::value_24_hour},{12,msg::value_12_hour_am_pm}};
    if(key=="date")return V{{0,"2026-11-25"},{1,"2026/11/25"},{2,"25/11/2026"},{3,"11/25/2026"}};
    if(key=="order")return V{{0,msg::time_first},{1,msg::date_first}};
    if(key=="zone")return V{{0,msg::use_system_time_zone},{1,"UTC"},{2,"Asia/Shanghai"},{3,"Asia/Tokyo"},{4,"Europe/Berlin"},{5,"America/New_York"}};
    if(key=="units")return V{{0,msg::binary_bytes_kib_s},{1,msg::decimal_bytes_kb_s},{2,msg::decimal_bits_kbit_s}};
    if(key=="locale")return V{{0,msg::combined},{1,"English"}};
    if(key=="keys")return V{{0,msg::vol_up_vol_down},{1,msg::vol_down_vol_up}};
    const auto d=deadline_state(a.backend);
    const int min=key=="connect"?d.connect:d.idle;
    V v{{-1,msg::default_setting}};
    for(int n:{600,900,1800,3600,7200,14400}) if(n>min)
        v.emplace_back(n,formatted(msg::arg0_min,a.settings().locale=="en",{std::to_string(n/60)}));
    v.emplace_back(0,msg::unlimited);return v;
}
int option_value(const Preferences& p,const std::string& k) {
    if(k=="hour")return p.hour;
    if(k=="date")return p.date_style;
    if(k=="order")return p.date_first;
    if(k=="zone")return p.zone;
    if(k=="units")return p.units;
    if(k=="locale")return p.locale=="en";
    if(k=="keys")return p.reverse;
    return k=="connect"?p.connect:p.idle;
}
void assign_option(Preferences& p,const std::string& k,int n) {
    if(k=="hour")p.hour=n;
    else if(k=="date")p.date_style=n;
    else if(k=="order")p.date_first=n;
    else if(k=="zone")p.zone=n;
    else if(k=="units")p.units=n;
    else if(k=="locale")p.locale=n?"en":"zh-CN";
    else if(k=="keys")p.reverse=n;
    else if(k=="connect")p.connect=n;
    else if(k=="idle")p.idle=n;
}
}

std::string App::activation() const {
    return page+":"+argument+":"+std::to_string(focus)+":"+std::to_string(dialog)+":"+dialog_action+":"+dialog_argument+
        ":"+std::to_string(dialog_focus)+":"+choice+":"+std::to_string(choice_focus)+":"+
        std::to_string(editing)+":"+std::to_string(log_back)+":"+std::to_string(log_back?0:log_cursor)+":"+
        std::to_string(actions.size())+":"+std::to_string(language_save.valid())+":"+save_error+":"+
        (page=="LOG_VIEW"?std::string{}:backend.task.id+":"+backend.task.status);
}

void App::change_settings(Preferences next,bool initial,bool deadline_authorized) {
    if(!initial && !pending_save && same_settings(next,settings()) && save_error.empty()) return;
    if(!initial && !deadline_authorized && expired_change(backend,next)){
        deadline_confirmation=next;dialog=true;dialog_focus=1;
        dialog_title=msg::save_a_timeout_that_has_already_elapsed;
        dialog_body=msg::the_timer_is_already_past_the_new_limit_if;
        pending_save.reset();
        return;
    }
    next.language_selected=true;
    uncommitted=next;
    pending_save=next;
    save_error.clear();
    retry_save_at=0;
    language_save_initial=initial;
    if (language_save.valid())return;
    auto p=*pending_save;pending_save.reset();
    saving_settings=p;
    try {
        language_save=std::async(std::launch::async,[p,initial,preview=backend.install_preview] {
            Preferences result;
            Backend target;target.install_preview=preview;
            if(!target.save_ui_preferences(p,initial,result))result.durable=false;
            return result;
        });
    } catch(const std::exception& e) {
        saving_settings.reset();save_error=e.what();notice=preference_notice(save_error,true);
    }
}

void App::render_settings() {
    Scene scene{*this};
    const auto p=settings();
    const std::array<std::array<const char*,4>,6> groups{{
        {"status",msg::status_icons,msg::only_icon_visibility_changes_connections_and_services_are_unaffected,"status"},
        {"time",msg::time_date,msg::changes_display_only_not_the_system_clock,"clock"},
        {"timeout",msg::reboot_on_timeout,msg::uses_upstream_timing_rules_reboots_to_fastboot_on_timeout,"reboot"},
        {"traffic",msg::live_traffic_units,msg::changing_units_updates_the_display_without_resetting_traffic_sampling,"traffic"},
        {"language",msg::system_language,msg::changing_the_interface_language_does_not_alter_other_display,"language"},
        {"keys",msg::button_controls,msg::the_navigation_direction_updates_on_every_screen_immediately_when,"keys"}
    }};
    std::string title=msg::settings,subtitle=msg::changes_apply_immediately_and_save_automatically;
    for(const auto& g:groups)if(argument==g[0]){title=g[1];subtitle=g[2];}
    if(argument=="save"){title=msg::settings_save_status;subtitle=msg::retry_saving_or_undo_unsaved_changes;}
    scene.title(title,subtitle);
    auto toggle=[&](const char* label,const char* icon,unsigned bit) {
        const bool on=bool(p.visible&bit);
        scene.item(label,on?msg::shown:msg::hidden,[this,bit]{auto q=settings();q.visible^=bit;change_settings(q);},
            icon,true,-1,false,on);
    };
    auto combo=[&](const char* label,const char* icon,const std::string& id) {
        auto items=options(*this,id);
        const int value=option_value(p,id);
        const auto at=std::find_if(items.begin(),items.end(),[value](const auto& item){return item.first==value;});
        const auto index=at==items.end()?0:static_cast<int>(at-items.begin());
        const auto desc=items[index].second;
        scene.item(label,desc,[this,id,items=std::move(items),index]{
            choice=id;choice_options=items;choice_focus=index;
        },icon,true,-1,true);
    };
    const auto d=deadline_state(backend);
    if(argument=="status"){
        toggle(msg::usb_network_icon,"usb",1);toggle(msg::bluetooth_icon,"bluetooth",2);
        toggle(msg::wi_fi_icon,"wifi",4);toggle(msg::ethernet_icon,"ethernet",8);
        toggle(msg::battery_percentage,"battery",16);toggle(msg::net_battery_power,"bolt",32);
        scene.info({{msg::connection_icons,msg::hiding_does_not_affect_connections_or_services},{msg::battery_percentage,msg::show_or_hide_together}});
    }else if(argument=="time"){
        toggle(msg::show_time,"clock",64);toggle(msg::show_date,"calendar",128);
        combo(msg::time_format,"hour","hour");toggle(msg::show_seconds,"seconds",256);
        combo(msg::date_format,"dateformat","date");combo(msg::display_order,"order","order");
        combo(msg::display_time_zone,"zone","zone");
        scene.info({{msg::applies_to,msg::all_recovery_screens},{msg::clock_display,clock_label(backend.telemetry,p)}});
    }else if(argument=="timeout"){
        combo(msg::connection_timeout,"reboot","connect");combo(msg::test_mode_host_idle_timeout,"seconds","idle");
    scene.info({{msg::reboot_destination,"fastboot"},{msg::current_mode,!d.known?msg::unknown:d.test?msg::test_mode:msg::normal_mode},
            {msg::test_mode_host_idle_timeout,d.test?msg::active_in_this_mode:msg::inactive_in_this_mode}});
    }else if(argument=="traffic"){
        combo(msg::traffic_units,"traffic","units");
        scene.info({{msg::upload_tx,rate_text(backend.telemetry.upload_bps,p.units)},
                    {msg::download_rx,rate_text(backend.telemetry.download_bps,p.units)}});
    }else if(argument=="language"){
        combo(msg::language_2,"language","locale");
        scene.info({{msg::current_language,p.locale=="en"?"English":msg::combined}});
    }else if(argument=="keys"){
        combo(msg::volume_button_direction,"swap","keys");
        scene.info({{msg::vol_plus,p.reverse?msg::next_item:msg::previous_selection},
                    {msg::vol_quieter,p.reverse?msg::previous_selection:msg::next_item},
                    {msg::power_key,msg::release_to_select}});
    }else if(argument!="save"){
        constexpr std::array summaries{msg::show_or_hide_status_information,msg::choose_formats_visibility_and_time_zone,
            msg::set_connection_and_test_time_limits,msg::choose_units_for_upload_and_download_speeds,msg::available_language_names,msg::swap_volume_button_navigation};
        for(std::size_t i=0;i<groups.size();++i){
            const auto& g=groups[i];
            scene.item(g[1],summaries[i],[this,id=g[0]]{navigate("SETTINGS",id);},g[3]);
        }
        scene.info({{msg::applies_to,msg::all_recovery_screens},{msg::current_language,p.locale=="en"?"English":msg::combined}});
    }
    const bool busy=language_save.valid();
    const bool failed=!save_error.empty();
    const bool locked=failed&&save_error.find("install.lock busy")!=std::string::npos;
    const auto state=busy?msg::saving:locked?msg::waiting_for_storage_changes_not_saved:failed?msg::save_failed_changes_not_saved:
        backend.preferences.durable?msg::saved_applied_on_all_screens:msg::save_not_confirmed;
    if(argument=="save"){
        scene.message(state,failed?msg::the_current_value_is_a_preview_saved_settings_are:msg::changes_apply_immediately_and_save_automatically,
            failed?"warning":"info");
        if(failed&&!busy) {
            const auto detail=preference_notice(save_error,true);
            scene.text(140,690,1200,92,detail.substr(detail.find('\n')+1),muted,25);
        }
    }else{
        scene.button(msg::settings_save_status,[this]{navigate("SETTINGS","save",0);},896,757,508,"save");
        scene.text(920,882,460,76,state,failed?neutral:muted,25);
    }
    if(failed&&!busy&&uncommitted){
        scene.button(msg::retry_saving,[this]{
            if(expired_change(backend,*uncommitted)){
                deadline_confirmation=*uncommitted;dialog=true;dialog_focus=1;
                dialog_title=msg::save_a_timeout_that_has_already_elapsed;dialog_body=msg::the_timer_is_already_past_the_new_limit_if;
            }else change_settings(*uncommitted);
        },896,960,508,"refresh");
        scene.button(msg::undo_changes,[this]{uncommitted.reset();pending_save.reset();save_error.clear();retry_save_at=0;},
            896,1068,508,"undo");
    }
}

bool App::step_reader(int direction,bool repeating) {
    if(report_detail()){
        const int next=std::clamp(report.top_line+direction,0,report.max_top());
        return std::exchange(report.top_line,next)!=next;
    }
    if(!log_layout.row_count)return false;
    if(log_source=="install-live")follow_install_log=false;
    if(log_back){
        if(repeating)return false;
        log_back=false;
        log_cursor=std::clamp(log_cursor,log_top,
            std::min(log_layout.row_count-1,log_top+log_view_lines-1));
        return true;
    }
    const int next=log_cursor+direction;
    if(next<0||next>=log_layout.row_count){
        if(repeating)return false;
        log_back=true;
        return true;
    }
    log_cursor=next;
    log_top=std::clamp(log_top,std::max(0,next-log_view_lines+1),
        std::min(next,log_layout.max_top()));
    return true;
}

bool App::cancel_reader_scroll() {
    const bool moved=reader_scroll.offset_px!=0;
    reader_scroll={};
    return moved;
}

double ReaderScroll::offset_at(double now) const {
    if(motion_duration<=0)return motion_to;
    const double u=std::clamp((now-motion_at)/motion_duration,0.,1.);
    double blend=u;
    if(motion==ReaderMotion::coast)blend=1-(1-u)*(1-u)*(1-u);
    else if(motion==ReaderMotion::resume)blend=u*(motion_tangent+(1-motion_tangent)*u*u);
    return std::lerp(motion_from,motion_to,blend);
}

double ReaderScroll::velocity_at(double now) const {
    if(motion_duration<=0||now<motion_at)return 0;
    const double age=now-motion_at;
    // While a held repeat is queued, a slow frame can cross its deadline.
    // Keep the last repeat velocity so a queued release can still start a coast.
    if(age>=motion_duration&&(!key||!next_step))return 0;
    const double u=std::min(1.,age/motion_duration);
    double slope=1;
    if(motion==ReaderMotion::coast)slope=3*(1-u)*(1-u);
    else if(motion==ReaderMotion::resume)slope=motion_tangent+3*(1-motion_tangent)*u*u;
    return (motion_to-motion_from)*slope/motion_duration;
}

double App::reader_bounded_top(double px,bool keep_selection) const {
    const int max_top=reader_max_top();
    if(keep_selection&&!report_detail()&&!log_back){
        const int lo=std::max(0,log_cursor-log_view_lines+1);
        const int hi=std::min(log_cursor,max_top);
        if(lo<=hi)return std::clamp(px,lo*double(log_line_px),hi*double(log_line_px));
    }
    return std::clamp(px,0.,max_top*double(log_line_px));
}

std::int64_t App::reader_visual_top_px() const {
    return static_cast<std::int64_t>(reader_bounded_top(reader_top()*double(log_line_px)-reader_scroll.offset_px));
}

namespace {
double reader_repeat_rate(double elapsed) {
    const double p=std::clamp(elapsed/reader_ramp,0.,1.);
    return reader_start_rate+(reader_max_rate-reader_start_rate)*p*p;
}
}

int App::log_leading_row(double visual,int direction) const {
    const double row=visual/log_line_px;
    const int edge=direction>0?static_cast<int>(std::floor(row))+log_view_lines-1:
                               static_cast<int>(std::ceil(row));
    return std::min(edge,std::max(0,log_layout.row_count-1));
}

bool App::catch_log_selection(double now) {
    auto& scroll=reader_scroll;
    if(!scroll.catch_rows)return false;
    const double visual=reader_bounded_top(log_top*double(log_line_px)-scroll.offset_at(now));
    const int gap=(log_leading_row(visual,scroll.direction)-log_cursor)*scroll.direction;
    if(gap<=0){scroll.catch_rows=0;return false;}
    const double p=std::clamp((now-scroll.catch_at)/reader_catch_time,0.,1.);
    const int due=static_cast<int>(scroll.catch_rows*p*p)-scroll.catch_done;
    const int steps=p>=1?gap:std::clamp(due,0,gap);
    if(steps<=0)return false;
    const int old_top=log_top;
    log_cursor+=steps*scroll.direction;
    clamp_log(*this,log_layout);
    const double shift=(log_top-old_top)*double(log_line_px);
    // Chase changes the selected row only. Keep the current and pending
    // viewport positions (and their speed) unchanged across the rebase.
    scroll.motion_from+=shift;
    scroll.motion_to+=shift;
    scroll.held_offset+=shift;
    scroll.catch_done+=steps;
    if(steps==gap||p>=1)scroll.catch_rows=0;
    return true;
}

bool App::begin_reader_scroll(int code,double now) {
    if(!reader_view()||(code!=volume_up&&code!=volume_down))return false;
    if(report_detail()&&(!report.loaded||report.id!=argument))compose();
    if(page=="LOG_VIEW"&&log_layout.source!=log_source)compose();
    auto& scroll=reader_scroll;
    const int direction=(code==volume_up?-1:1)*(settings().reverse?-1:1);
    const double position=reader_top()*double(log_line_px)-scroll.offset_at(now);
    const double visual=reader_bounded_top(position);
    const double raw_velocity=reduced_motion?0:-scroll.velocity_at(now);
    const bool at_bound=visual!=position||(visual==0&&raw_velocity<0)||
        (visual>=reader_max_top()*double(log_line_px)&&raw_velocity>0);
    const double velocity=at_bound?0:raw_velocity;
    const bool resume=scroll.coasting(now)&&velocity*direction>0;
    // Preserve the held ramp's elapsed time across the coast, then continue
    // ticking it immediately, including while the log selection catches up.
    const double repeat_at=resume?scroll.repeat_at+(now-scroll.motion_at):now+reader_delay;
    if(!resume){
        // The animation may have rested several rows beyond its anchor.
        // Rebase the next deliberate tap to what is on screen, preserving
        // the selected log row and any fractional pixel remainder.
        const auto top=static_cast<int>(std::round(reader_bounded_top(visual,true)/log_line_px));
        if(report_detail())report.top_line=top;
        else log_top=top;
    }
    const int old_top=reader_top();
    const bool stepped=step_reader(direction,resume);
    if(!stepped&&resume)return false; // A repeat at the boundary doesn't interrupt its final glide.
    const double base=reader_top()*double(log_line_px);
    scroll={};
    scroll.direction=direction;
    if(!stepped){
        const bool at_report_edge=report_detail()&&(direction>0?reader_top()==reader_max_top():reader_top()==0);
        scroll.motion_to=reduced_motion||at_report_edge?0:base-visual;
        scroll.offset_px=static_cast<int>(std::lround(scroll.motion_to));
        return base!=old_top*double(log_line_px)||visual!=base;
    }
    scroll.key=code;
    scroll.repeat_at=repeat_at;
    scroll.next_step=now+reader_delay;
    if(resume){
        scroll.motion_from=base-visual;
        scroll.motion_to=base-reader_bounded_top(visual+direction*log_line_px,true);
        const double distance=std::abs(scroll.motion_to-scroll.motion_from);
        if((scroll.motion_from-scroll.motion_to)*direction>0){
            // Match the residual speed at the start and the preserved ramp at
            // the end, with a quadratic speed transition over one row.
            const double speed=std::abs(velocity);
            const double target=std::max(speed,reader_repeat_rate(now-repeat_at)*log_line_px);
            scroll.motion_duration=3*distance/(2*speed+target);
            scroll.next_step=now+scroll.motion_duration;
            scroll.motion=ReaderMotion::resume;
            scroll.motion_at=now;
            scroll.motion_tangent=speed*scroll.motion_duration/distance;
        }else scroll.next_step=now+1/reader_repeat_rate(now-repeat_at);
        if(!report_detail()){
            scroll.catch_at=now;
            scroll.catch_rows=std::max(0,(log_leading_row(visual,direction)-log_cursor)*direction);
        }
    }else{
        const auto landing=reader_bounded_top(visual+(reader_top()-old_top)*double(log_line_px),true);
        scroll.motion_from=scroll.motion_to=reduced_motion?0:base-landing;
    }
    scroll.held_offset=scroll.motion_to;
    scroll.offset_px=static_cast<int>(std::lround(scroll.offset_at(now)));
    return true;
}

void App::end_reader_scroll(int code,double now) {
    auto& scroll=reader_scroll;
    if(scroll.key!=code)return;
    const double base=reader_top()*double(log_line_px);
    const double from=reader_bounded_top(base-scroll.offset_at(now));
    const double velocity=reduced_motion?0:-scroll.velocity_at(now);
    scroll.key=0;
    scroll.next_step=0;
    scroll.catch_rows=0;
    // The logical row and its anchor stay fixed. Ease out the viewport from
    // the current velocity: v(t) = v0 * (1 - t / T)^2.
    const double stop=reader_bounded_top(from+velocity*reader_coast_time/3,true);
    const auto distance=stop-from;
    scroll.motion=ReaderMotion::coast;
    scroll.motion_from=base-from;
    scroll.motion_at=now;
    if(distance*velocity>0){
        scroll.motion_to=base-stop;
        scroll.motion_duration=3*distance/velocity;
    }else{
        scroll.motion_to=scroll.motion_from;
        scroll.motion_duration=0;
    }
}

bool App::tick_reader_scroll(double now) {
    auto& scroll=reader_scroll;
    if(!reader_view())return cancel_reader_scroll();
    bool changed=false;
    if(scroll.key&&scroll.next_step>0&&now>=scroll.next_step){
        const double visual=reader_bounded_top(reader_top()*double(log_line_px)-scroll.offset_at(now));
        if(step_reader(scroll.direction,true)){
            changed=true;
            const double interval=1/reader_repeat_rate(now-scroll.repeat_at);
            scroll.next_step=now+interval; // Never replay missed steps after a slow frame.
            const double base=reader_top()*double(log_line_px);
            scroll.motion_from=base-visual;
            scroll.motion_to=base-reader_bounded_top(base-scroll.held_offset,true);
            scroll.motion_at=now;
            scroll.motion_duration=!reduced_motion&&scroll.motion_from!=scroll.motion_to?interval:0;
            scroll.motion=ReaderMotion::step;
        }else scroll.next_step=0;
    }
    if(scroll.key&&scroll.catch_rows)changed|=catch_log_selection(now);
    const int offset=reduced_motion?0:static_cast<int>(std::lround(scroll.offset_at(now)));
    const bool stopped=scroll.motion==ReaderMotion::coast&&scroll.motion_duration>0&&!scroll.animating(now);
    if(stopped){
        scroll.motion=ReaderMotion::idle;
        scroll.motion_duration=0;
    }
    return (std::exchange(scroll.offset_px,offset)!=offset)||changed||stopped;
}

void App::key(int code) {
    if (code != volume_up && code != volume_down && code != power_key) return;
    cancel_reader_scroll();
    compose();
    const int direction=(code==volume_up?-1:1)*(settings().reverse?-1:1);
    if(!choice.empty()){
        const auto key=choice;
        const auto opts=choice_options;
        const int count=static_cast<int>(opts.size())+1;
        if(code!=power_key){choice_focus=(choice_focus+direction+count)%count;compose();return;}
        choice.clear();
        if(choice_focus>=0 && choice_focus<count-1){
            auto next=settings();assign_option(next,key,opts[choice_focus].first);
            change_settings(next);
        }
        compose();return;
    }
    if (dialog) {
        if (code != power_key) { dialog_focus = 1 - dialog_focus; compose(); return; }
        if (dialog_focus == 1) { back(); compose(); return; }
        if(deadline_confirmation){
            auto next=*deadline_confirmation;
            deadline_confirmation.reset();dialog=false;change_settings(next,false,true);compose();return;
        }
        const std::string action = dialog_action, arg = dialog_argument;
        const std::string key = action == "bootmode" ? arg :
            action == "probe" ? "probe" : action + ":" + arg;
        const auto operation = request(backend, key, settings().locale=="en");
        dialog = false;
        dialog_focus = 0;
        dialog_action.clear();
        dialog_argument.clear();
        std::string error;
        if (!operation.reason.empty() || operation.action != action ||
            operation.argument != arg || !backend.start(action, arg, error)) {
            notice = !operation.reason.empty() ? operation.reason :
                error.empty() ? msg::the_task_did_not_start_check_the_requirements_and : error;
            navigate("UNAVAILABLE", key, 0);
        } else {
            notice.clear();
            navigate(action == "power" ? "EXIT" : action == "hash" ? "HASH" :
                action == "mount" || action == "umount" ? "MOUNT" : "RUNNING", key, 1);
        }
        compose();
        return;
    }
    if (editing >= 0 && (page == "DISPLAY" || page == "LAYER_EDIT")) {
        if (code == power_key) {
            editing = -1;
            page = "DISPLAY";
        } else {
            const std::array<int*, 4> values{&draft.bg, &draft.button, &draft.text, &draft.icon};
            constexpr std::array bounds{16, 12, 8, 8};
            if (editing < static_cast<int>(values.size()))
                *values[editing] = std::clamp(*values[editing] +
                    (code == volume_up ? 2 : -2), 0, bounds[editing]);
        }
        compose();
        return;
    }
    if (reader_view()) {
        if (code == power_key) {
            if (report_detail()||log_back) back();
            else {
                log_back = true;
                if(log_source=="install-live"){
                    follow_install_log=true;log_top=log_layout.max_top();
                    log_cursor=std::max(0,log_layout.row_count-1);
                }
            }
        } else step_reader(direction);
        compose();
        return;
    }
    if (code == power_key) {
        if (focus >= 0 && focus < static_cast<int>(actions.size()) && actions[focus]) {
            const auto action = actions[focus];
            action();
        }
    } else if (!actions.empty()) {
        const int count = static_cast<int>(actions.size());
        for (int step = 1; step <= count; ++step) {
            const int next = (focus + direction * step + count) % count;
            if (actions[next]) { focus = next; break; }
        }
    }
    compose();
}

void App::compose() {
    if (language_save.valid() &&
        language_save.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Preferences result;
        try { result = language_save.get(); }
        catch (const std::exception& error) { result.error = error.what(); }
        catch (...) { result.error = msg::no_language_save_result_was_received; }
        const bool matches=saving_settings&&(language_save_initial?
            result.locale==saving_settings->locale:same_settings(result,*saving_settings));
        saving_settings.reset();
        if (!matches || !result.durable || !result.language_selected || (result.locale != "zh-CN"&&result.locale!="en") ||
            (result.storage != "data" && result.storage != "cache" &&
             !(backend.install_preview&&result.storage=="preview")) || !result.error.empty()) {
            save_error = result.error.empty() ? msg::no_valid_language_save_result_was_received : result.error;
            if(language_save_initial){backend.preferences.error=save_error;notice=preference_notice(save_error,true);}
            if(save_error.find("install.lock busy")!=std::string::npos && animation_phase>0)
                retry_save_at=animation_phase+2;
        } else {
            backend.preferences = std::move(result);
            if(!pending_save) uncommitted.reset();
            notice.clear();
            if (!language_save_initial) {
                if (page == "LANGUAGE" && argument == "settings") back();
            } else {
                open_home(task_present(backend.task)?8:0);
            }
        }
    }
    if(pending_save&&!language_save.valid()&&!dialog&&choice.empty())change_settings(*pending_save,language_save_initial);
    if(retry_save_at>0 && animation_phase>=retry_save_at && uncommitted && !language_save.valid() &&
       (!language_save_initial||page=="LANGUAGE")&&
       (!language_save_initial||!backend.task.locked||backend.install_preview)) {
        if(expired_change(backend,*uncommitted) && !language_save_initial) retry_save_at=0;
        else {
            if(language_save_initial)notice=msg::saving_language_settings;
            change_settings(*uncommitted,language_save_initial);
        }
    }
    nodes.clear();
    actions.clear();
    if (page == "RUNNING" && !task_running(backend.task)) {
        if (task_failed(backend.task)) page = "ERROR";
        else if (task_succeeded(backend.task)) page = "SUCCESS";
    }
    if (page == "EXIT" && task_failed(backend.task)) page = "ERROR";
    Scene scene{*this};
    const bool en=settings().locale=="en";
    const auto tr=[&](std::string_view value){return std::string(localized(value,en));};
    const auto home = [this] {
        open_home();
    };
    const auto go = [this](const std::string& id, const std::string& arg = "", int initial = 1) {
        navigate(id, arg, initial);
    };
    const auto detail = [this](const std::string& key) { navigate("TASK_DETAIL", key); };
    const auto logs = [this](const std::string& source) { navigate("LOG_VIEW", source, 0); };
    const auto& telemetry = backend.telemetry;

    if (page == "SETTINGS") {
        render_settings();
    } else if (page == "LANGUAGE") {
        const bool initial = argument != "settings";
        const bool return_to_log=initial&&first_install_entry;
        scene.title(msg::language, initial ? msg::choose_a_language_for_recovery :
            msg::choose_a_language_to_save, !initial||return_to_log);
        for(const auto& [name,lang]:std::array<std::pair<const char*,const char*>,2>{{{"English","en"},{msg::combined,"zh-CN"}}})
            scene.item(name,lang,[this,initial,lang]{
                if(language_save.valid())return;
                auto p=settings();p.locale=lang;
                if(initial&&backend.task.locked&&!backend.install_preview){
                    uncommitted=p;language_save_initial=true;save_error.clear();
                    retry_save_at=animation_phase+1;notice=msg::your_language_will_be_saved_when_installation_releases_the;
                }else{
                    notice=msg::saving_language_settings;change_settings(p,initial);
                }
            },"language");
        scene.info({{msg::available_language, msg::available_language_names},
            {msg::settings_storage, backend.install_preview?msg::preview_session_ram:preference_storage(backend.preferences.storage)},
            {msg::save_status, backend.preferences.durable ? msg::saved_to_device : msg::save_not_confirmed}});
        scene.text(36, 724, 1368, 200, notice.empty() ?
            msg::use_volume_to_choose_press_power_to_save_and : notice,
            notice.empty() || language_save.valid() ? muted : negative);
    } else if (page == "BOOT") {
        scene.title(msg::recovery_maintenance, msg::starting_recovery, false);
        scene.message(msg::preparing_recovery,
            msg::loading_settings_and_checking_the_installed_system, "info", 354, 316);
    } else if (page == "BOOT_ERROR") {
        scene.title(msg::display_not_ready, msg::home_is_unavailable_check_the_recovery_logs_from_the, false);
        scene.message(msg::display_check_failed, notice.empty() ?
            msg::both_displays_are_not_confirmed_ready_if_the_screens : notice, "error", 354);
        scene.text(140, 740, 1180, 160, msg::the_existing_safety_timeouts_still_apply, muted);
        scene.button(msg::power_startup, [go] { go("POWER"); }, 36, 1030, 650, "power");
    } else if (page == "HOME") {
        scene.title(msg::recovery_maintenance, msg::choose_what_to_view_or_do, false);
        scene.item(msg::system_status, msg::system_hardware_and_services, [go] { go("STATUS"); }, "info");
        scene.item(msg::install_maintain, msg::install_images_build_or_restore_the_factory_system, [go] { go("MAINT"); }, "tools");
        scene.item(msg::host_connection, msg::check_usb_authentication_and_install_inputs, [go] { go("HOST"); }, "host");
        scene.item(msg::storage_read_only_checks, msg::inspect_partitions_compute_hashes_or_mount_read_only, [go] { go("STORAGE"); }, "storage");
        scene.item(msg::diagnostics_logs, msg::view_hardware_reports_and_logs, [go] { go("DIAG"); }, "report");
        scene.item(msg::display_depth, msg::adjust_and_preview_stereo_depth, [go] { go("DISPLAY"); }, "stereo");
        scene.item(msg::settings, msg::status_bar_time_timeout_traffic_language_and_buttons, [go] { go("SETTINGS"); }, "settings");
        scene.item(msg::power_startup, msg::choose_a_startup_mode_restart_or_shut_down, [go] { go("POWER"); }, "power");
        const bool all_ro = !backend.partitions.empty() &&
            std::all_of(backend.partitions.begin(), backend.partitions.end(),
                [](const Partition& item) { return item.readonly; });
        scene.info({{msg::environment, observed(telemetry.kernel)},
            {msg::block_devices, backend.partitions.empty() ? msg::no_partition_data : all_ro ? msg::listed_partitions_are_read_only : msg::some_partitions_are_not_read_only},
            {msg::host, usb_label(telemetry.usb,en)}, {msg::stereo_display, msg::value_2160_2160_per_eye},
            {msg::task, task_present(backend.task) ? task_status(backend.task.status) : msg::waiting_for_the_host}});
        const bool install_log = first_install_entry || (!seen_install_task.empty() && backend.task.id == seen_install_task);
        scene.button(install_log?msg::view_live_install_log:msg::view_current_task, [go,install_log] {
            if(install_log)go("LOG_VIEW","install-live",0);
            else go("CURRENT_TASK");
        }, 920, 920, 456);
        scene.text(920, 1034, 456, 112, msg::going_to_home_does_not_start_an_install_or, muted, 25);
    } else if (page == "CURRENT_TASK") {
        const auto& task = backend.task;
        scene.title(msg::current_task_2, task_present(task) ? install_stage_detail(observed(task.name),en) :
            msg::waiting_for_the_host_to_start_installation, true, true);
        const int overview = scene.bind([logs] { logs("task"); });
        const bool chosen = scene.selected(overview);
        scene.box(36, 332, 1368, 568, view, Layer::button, chosen ? selection : rule, chosen ? 4 : 2);
        scene.icon(64, 356, task_failed(task) ? "error" : "report", task_failed(task) ? negative : ink, 52);
        scene.text(140, 352, 1224, 64, msg::task_overview, ink, 36, true);
        std::vector<std::string> lines;
        if (!task_present(task)) {
            lines = {msg::continue_installation_from_the_host_using_the_existing_workflow, msg::flashing_recovery_does_not_install_the_system,
                msg::no_image_write_task_has_been_received, msg::this_view_updates_when_installation_inputs_and_stage_status,
                msg::go_to_home_to_check_connections_reports_or_display};
        } else {
            const bool en=settings().locale=="en";
            lines = {stage_summary(task,en), formatted(msg::task_arg0_arg1,en,{
                    install_stage_detail(observed(task.name),en),
                    std::string(localized(task_failed(task) ? msg::failed : task_succeeded(task) ? msg::completed :
                        task_running(task) ? msg::in_progress : msg::waiting_2,en))}),
                msg::percentages_are_shown_only_when_reliable_progress_is_available};
            if (!task.detail.empty()) lines.push_back(install_stage_detail(task.detail,en));
            if (!task.error.empty()) lines.push_back(install_stage_detail(task.error,en));
            const auto output = log_lines(backend,"task",en);
            const auto first = output.size() > 3 ? output.size() - 3 : 0;
            for (std::size_t i = first; i < output.size() && lines.size() < 7; ++i)
                lines.push_back(output[i]);
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(7, lines.size()); ++i)
            scene.text(64, 440 + i * 54, 1280, 50, lines[i], i ? muted : ink);
        scene.text(64, 850, 1280, 44, msg::select_the_overview_with_the_volume_buttons_then_release, muted, 25);
        scene.progress(36, 904, 1368);
        if (!task_present(task))
            scene.button(msg::view_host_connection, [go] { go("HOST"); });
        else scene.button(msg::back_to_home, home, 472, 1018, 420);
    } else if (page == "STATUS") {
        scene.title(msg::system_status, msg::view_the_current_state_of_recovery);
        scene.item(msg::hardware_inventory, msg::browse_collected_hardware_information, [go] { go("HARDWARE"); }, "report");
        scene.item(msg::service_status, msg::inspect_existing_services_read_only, [go] { go("SERVICES"); }, "host");
        scene.item(msg::recovery_timeouts, msg::check_connection_and_test_timeouts_and_exit_conditions, [go] { go("DEADLINE"); }, "info");
        scene.item(msg::system_language, "", [go] { go("SETTINGS","language"); }, "language");
        scene.info({{msg::kernel_system, observed(telemetry.kernel)},
            {msg::uptime, uptime_label(telemetry.uptime,en)},
            {msg::battery, (telemetry.capacity ? std::to_string(*telemetry.capacity) + "%" : tr(msg::unknown)) +
                " · " + tr(battery_status(telemetry.battery_status))},
            {msg::memory, observed(telemetry.memory)},
            {msg::detected_partitions, formatted(msg::partitions_detected_arg0,en,{std::to_string(backend.partitions.size())})}});
    } else if (report_detail()) {
        draw_report(scene);
    } else if (page == "HARDWARE" || page == "REPORT") {
        scene.title(page == "HARDWARE" ? msg::hardware_inventory : msg::report_categories,
            msg::choose_a_report_from_this_recovery_session);
        scene.paged = true;
        for (const auto& report : reports)
            scene.item(report.name, formatted(msg::arg0_read_only,en,{std::string(report.id)}),
                [go, id = std::string(report.id)] { go("REPORT", id); }, "report");
        const int first = std::max(0, (focus - 1) / 7) * 7;
        scene.text(36, 1120, 820, 40, formatted(msg::categories_arg0_arg1_of_arg2_use_volume_to_browse,en,{
            std::to_string(first+1),std::to_string(std::min(first+7,static_cast<int>(reports.size()))),
            std::to_string(reports.size())}), muted, 25);
        scene.info({{msg::report_type, msg::existing_files_read_only}, {msg::contents, msg::system_hardware_inventory},
            {msg::functional_verification, msg::nodes_detected_functions_untested}});
    } else if (page == "SERVICES") {
        scene.title(msg::service_status_2, msg::view_services_without_starting_stopping_or_configuring_them);
        const auto status = trimmed(backend.read("/run/overpressure/services", 65536));
        if (!status.empty()) {
            const auto lines = wrap(status, 1312, 29.333);
            for (std::size_t i = 0; i < std::min<std::size_t>(7, lines.size()); ++i)
                scene.text(64, 350 + i * 92, 1312, 84, lines[i], i ? muted : ink);
        } else {
            std::vector<std::string> lines{msg::live_status_is_unavailable_these_logs_do_not_confirm};
            for (const auto* name : {"udhcpd", "dropbear", "telnetd", "httpd", "recovery-watch"}) {
                const auto output = trimmed(backend.read("/run/overpressure/svc-" + std::string(name) + ".log", 16384));
                const auto entries = lines_of(output);
                lines.push_back(formatted(msg::arg0_last_entry_arg1,en,{name,
                    output.empty() ? tr(msg::no_readable_entry) : entries.back()}));
            }
            scene.content(lines);
        }
        scene.button(msg::view_service_logs, [logs] { logs("services"); }, 36, 1030, 650, "report");
    } else if (page == "DEADLINE") {
        scene.title(msg::recovery_timeouts, msg::view_this_image_s_timeouts_button_presses_do_not);
        const auto d=deadline_state(backend);
        const auto desc=[&](int value,int base){
            if(value==0)return std::string(localized(msg::unlimited,en));
            if(value<=base)return std::string(localized(msg::default_setting,en));
            return formatted(msg::arg0_min,en,{std::to_string(value/60)});
        };
        scene.content({formatted(msg::connection_timeout_arg0,en,{desc(backend.preferences.connect,d.connect)}),
            formatted(msg::test_timeout_arg0,en,{d.test?desc(backend.preferences.idle,d.idle):tr(msg::inactive_in_this_mode)}),
            msg::recovery_reboots_to_fastboot_on_timeout, msg::headset_buttons_do_not_extend_the_host_timeout});
        scene.button(msg::reboot_on_timeout,[go]{go("SETTINGS","timeout");},36,1018,650,"reboot");
    } else if (page == "HOST") {
        scene.title(msg::host_connection, msg::this_page_shows_connection_and_input_status_provide_installation);
        const auto net = backend.read("/run/overpressure/net.env");
        scene.content({formatted(msg::usb_link_arg0,en,{usb_label(telemetry.usb,en)}),
            formatted(msg::interface_arg0,en,{tr(observed(field(net,"IFACE")))}),
            formatted(msg::network_address_arg0,en,{tr(observed(field(net,"IP")))}),
            formatted(msg::last_host_contact_seconds_since_boot_arg0,en,{tr(observed(trimmed(backend.read("/run/overpressure/host-contact"))))}),
            msg::authentication_no_live_result_a_link_or_traffic_does,
            msg::install_inputs_this_ui_cannot_verify_install_inputs_use});
    } else if (page == "MAINT") {
        scene.title(msg::install_maintain, msg::check_the_inputs_and_review_the_consequences_before_proceeding);
        scene.item(msg::install_system_images, msg::overwrites_existing_system_partitions, [go] { go("INPUTS", "install"); }, "storage");
        scene.item(msg::on_device_build_logs, msg::requires_sources_tools_and_a_data_partition, [go] { go("BUILD_MENU"); }, "tools");
        scene.item(msg::format_for_linux_data, msg::erases_existing_apps_and_user_data, [go] { go("INPUTS", "format"); }, "delete");
        scene.item(msg::install_public_keys_install_id, msg::requires_host_inputs_and_pico4_data, [go] { go("INPUTS", "keys"); }, "unlock");
        scene.item(msg::device_system_assets, msg::update_backed_up_assets_or_verify_checksums, [go] { go("STOCK_MENU"); }, "archive");
        scene.item(msg::factory_restore_partition_clearing, msg::restore_the_factory_system_or_clear_partitions, [go] { go("RESTORE_MENU"); }, "undo");
        scene.item(msg::next_boot_mode, msg::choose_the_next_boot_mode_without_restarting, [go] { go("BOOT_MODE"); }, "power");
    } else if (page == "BUILD_MENU") {
        scene.title(msg::on_device_build, msg::build_on_this_device_or_view_existing_build_logs);
        scene.item(msg::start_on_device_build, msg::check_sources_tools_and_the_data_partition_first, [go] { go("INPUTS", "build"); }, "tools");
        scene.item(msg::view_build_logs, msg::read_only_starts_no_build_or_mount, [logs] { logs("build"); }, "report");
        scene.info({{msg::build_location, msg::existing_pico4_data}, {msg::inputs_tools, msg::no_verification_result},
            {msg::retrieve_files, msg::use_the_existing_host_connection}});
    } else if (page == "STOCK_MENU") {
        scene.title(msg::device_system_assets, msg::use_only_a_backup_from_this_device);
        scene.item(msg::update_asset_copy, msg::check_the_archive_and_file_hashes_first, [go] { go("INPUTS", "stock"); }, "storage");
        scene.item(msg::verify_asset_copy, msg::read_only_check_no_repairs, [go] { go("INPUTS", "stockhash"); }, "report");
        scene.info({{msg::factory_files, msg::original_files_on_this_device_s_persist}, {msg::asset_copy, msg::separate_area_on_pico4_data},
            {msg::replacement_condition, msg::all_checks_must_pass}});
    } else if (page == "RESTORE_MENU") {
        scene.title(msg::factory_restore_partition_clearing, msg::factory_restore_requires_a_backup_from_this_device);
        scene.item(msg::restore_the_factory_system, msg::replaces_the_system_and_makes_existing_user_data_inaccessible, [go] { go("INPUTS", "restore"); }, "storage");
        scene.item(msg::clear_start_of_userdata, msg::makes_the_existing_file_system_unusable, [go] { go("INPUTS", "wipeuser"); }, "prepare");
        scene.item(msg::clear_start_of_metadata, msg::erases_existing_contents_at_the_start_of_the_partition, [go] { go("INPUTS", "wipemeta"); }, "clear");
        scene.info({{msg::on_device, msg::restore_system_and_clear_specified_areas}, {msg::on_host, msg::flash_factory_recovery_via_fastboot},
            {msg::factory_calibration, msg::persist_remains_read_only}});
    } else if (page == "INPUTS" || page == "TASK_DETAIL" || page == "REVIEW" || page == "CONFIRM") {
        const auto operation = request(backend, argument,en);
        if (page == "INPUTS") {
            scene.title(msg::inputs_requirements, operation.name);
            scene.content(operation.inputs, 104);
            scene.text(64, 820, 1280, 116, operation.reason.empty() ?
                msg::review_the_consequences_before_confirming_requirements_are_checked_again : operation.reason, muted);
            scene.button(operation.reason.empty() ? msg::view_task_details : msg::view_unmet_requirements,
                [go, key = argument, unavailable = !operation.reason.empty()] {
                    go(unavailable ? "UNAVAILABLE" : "TASK_DETAIL", key);
                });
        } else if (page == "TASK_DETAIL") {
            scene.title(operation.name, msg::details_only_no_action_is_run_from_this_page);
            scene.message(msg::what_this_does, operation.effect);
            scene.text(64, 646, 1280, 100, formatted(msg::tool_arg0,en,{tr(operation.module)}), muted, 25);
            std::string steps;
            for (const auto& step : operation.steps) {
                if (!steps.empty()) steps += " → ";
                steps += tr(step);
            }
            scene.text(64, 774, 1280, 150, steps.empty() ?
                msg::no_usable_inputs_are_available_so_steps_and_verification : formatted(msg::steps_arg0,en,{steps}));
            scene.button(msg::review_before_starting, [go, key = argument] { go("REVIEW", key); });
        } else {
            scene.title(msg::review_before_starting, formatted(msg::arg0_you_can_still_go_back_before_confirming,en,{tr(operation.name)}));
            scene.content(operation.inputs.empty() ?
                std::vector<std::string>{msg::no_usable_inputs} : operation.inputs);
            scene.text(64, 768, 1280, 198, operation.effect);
            scene.button(msg::review_confirmation, [this, key = argument,en] {
                const auto current = request(backend, key,en);
                if (!current.reason.empty() || current.action.empty()) {
                    notice = current.reason;
                    navigate("UNAVAILABLE", key, 0);
                } else confirm(formatted(msg::confirm_action_arg0,en,{std::string(localized(current.name,en))}),
                               current.effect, current.action, current.argument);
            }, 36, 1018, 650, "next");
        }
    } else if (page == "UNAVAILABLE") {
        const auto operation = request(backend, argument,en);
        scene.title(msg::requirements_not_met, argument.empty() ? msg::action_unavailable : operation.name);
        scene.message(msg::cannot_proceed, !notice.empty() ? notice : !operation.reason.empty() ?
            operation.reason : msg::required_inputs_or_checks_are_missing, "warning", 320, 310);
        scene.text(140, 710, 1180, 118,
            msg::go_back_to_check_status_or_provide_the_missing, muted);
        scene.button(msg::view_host_connection, [go] { go("HOST"); });
    } else if (page == "RUNNING" || page == "HASH" || page == "SUCCESS" || page == "ERROR" || page == "MOUNT") {
        const auto& task = backend.task;
        const auto operation = request(backend, argument,en);
        const bool failed = task_failed(task);
        const bool success = task_succeeded(task) && !task_running(task);
        scene.title(!task_present(task) ? msg::current_task_2 : failed ? msg::task_failed : success ?
            page == "HASH" ? msg::hash_computed : page == "MOUNT" ? msg::mount_operation_ended : msg::task_completed :
            page == "HASH" ? msg::computing_hash : msg::task_in_progress,
            task_present(task) ? install_stage_detail(observed(task.name),en) : msg::waiting_for_task_status);
        if (!task_present(task)) {
            scene.message(msg::no_task_status_yet, msg::opening_this_page_does_not_start_a_task_progress);
        } else if (failed) {
            scene.message(msg::the_task_could_not_finish, install_stage_detail(task.error.empty()?observed(task.detail):task.error,en), "error", 332);
            scene.text(64, 690, 1280, 176, formatted(msg::read_only_protection_restored_arg0_check_the_logs_inputs,en,
                {tr(task.readonly_restored ? msg::confirmed : msg::not_confirmed)}), muted);
        } else if (success) {
            scene.message(page == "HASH" ? msg::computation_complete : msg::task_process_exited,
                formatted(msg::arg0_read_only_protection_restored_arg1,en,
                    {install_stage_detail(observed(task.detail),en),tr(task.readonly_restored ? msg::confirmed : msg::not_confirmed)}),
                task.readonly_restored ? "ok" : "warning", 332, 286);
            if (page == "HASH")
                scene.text(64, 650, 1280, 176, msg::see_the_task_output_for_the_sha_256_hash, muted);
            else if (page == "MOUNT") {
                const auto at = argument.find(':');
                const auto* item = partition(backend, at == std::string::npos ? "" : argument.substr(at + 1));
                scene.text(64, 650, 1280, 176, item ?
                    formatted(msg::current_mount_arg0_block_device_arg1,en,{
                        item->mount.empty()?tr(msg::no_mount_detected):item->mount,
                        tr(item->readonly?msg::read_only_2:msg::not_read_only)}) :
                    msg::current_partition_status_is_unavailable_the_mount_result_could, muted);
            } else scene.text(64, 650, 1280, 176, operation.effect, muted);
        } else {
            scene.message(stage_summary(task,en),install_stage_detail(observed(task.detail),en), "info", 332, 240);
            scene.text(64, 690, 1280, 176,
                msg::changing_views_does_not_stop_the_task_conflicting_tasks, muted);
        }
        scene.progress(36, 904, 1368);
        scene.button(success || failed ? msg::back_to_home_2 : msg::back_to_overview,
            [go, home, success, failed] {
                if (success || failed) home();
                else go("CURRENT_TASK");
            }, 472, 1018, 420);
        if (failed) scene.button(msg::recheck_inputs, [go, key = argument] { go("INPUTS", key); },
            908, 1018, 496);
    } else if (page == "STORAGE") {
        scene.title(msg::storage_read_only_checks, msg::choose_a_partition_to_inspect_unreadable_capacities_are_marked);
        scene.paged = true;
        for (const auto& item : backend.partitions) {
            if (!valid_partition_name(item.name)) continue;
            scene.item(item.name, tr(bytes_text(item.bytes,en)) + " · " + tr(observed(item.filesystem)),
                [go, name = item.name] { go("PARTITION", name); }, "storage");
        }
        if (!scene.row) scene.message(msg::no_partition_information_yet, msg::wait_for_partition_information_or_go_back_and_try, "info", 332);
        else {
            const int first = std::max(0, (focus - 1) / 7) * 7;
            scene.text(36, 1120, 820, 40, formatted(msg::partitions_arg0_arg1_of_arg2_use_volume_to_browse,en,{
                std::to_string(first+1),std::to_string(std::min(first+7,scene.row)),std::to_string(scene.row)}), muted, 25);
            scene.info({{msg::capacity_source, msg::block_device}, {msg::access, msg::read_only_inventory},
                {msg::action, msg::select_a_partition_first}});
        }
    } else if (page == "PARTITION") {
        const auto* item = partition(backend, argument);
        scene.title(item ? item->name : msg::partition_unavailable, msg::view_partition_details_and_current_mount_status);
        if (!item) scene.message(msg::partition_not_found, msg::go_back_and_select_the_partition_again_the_old, "warning");
        else {
            scene.content({formatted(msg::capacity_arg0_arg1_b,en,{tr(bytes_text(item->bytes,en)),std::to_string(item->bytes)}),
                formatted(msg::type_arg0,en,{tr(observed(item->filesystem))}),
                formatted(msg::block_device_arg0,en,{tr(observed(item->device))}),
                formatted(msg::write_protection_arg0_mount_arg1,en,{
                    tr(item->readonly?msg::read_only_2:msg::not_read_only),
                    item->mount.empty()?tr(msg::no_mount_detected):item->mount})}, 94);
            scene.text(64, 754, 1280, 116, mountable(*item) ?
                msg::read_only_mount_options_ext4_uses_ro_noload_f2fs :
                msg::read_only_mounting_is_unavailable_for_this_file_system, muted);
            scene.button(msg::compute_partition_sha_256, [detail, name = item->name] { detail("hash:" + name); },
                36, 928, 650);
            if (!item->mount.empty() || mountable(*item))
                scene.button(item->mount.empty() ? msg::mount_read_only : msg::unmount_inspection_mount,
                    [detail, key = std::string(item->mount.empty() ? "mount:" : "umount:") + item->name] {
                        detail(key);
                    }, 702, 928, 702);
        }
    } else if (page == "DIAG") {
        scene.title(msg::diagnostics_logs, msg::collect_information_or_view_reports_and_logs_no_hardware);
        scene.item(msg::collect_hardware_info, msg::refreshes_inventory_without_testing_hardware, [detail] { detail("probe"); }, "refresh");
        scene.item(msg::view_hardware_reports, msg::browse_read_only_reports_by_category, [go] { go("REPORT"); }, "report");
        scene.item(msg::view_logs, msg::boot_kernel_service_pstore_and_task_logs, [go] { go("LOGS"); }, "log");
        scene.info({{msg::data_sources, msg::proc_sysfs_device_tree}, {msg::scope, msg::inventory_and_diagnostics},
            {msg::functional_verification, msg::nodes_detected_functions_untested}});
    } else if (page == "LOGS") {
        scene.title(msg::logs, msg::back_is_selected_by_default_release_power_to_return);
        for (const auto& source : {std::string("/run/overpressure/boot.log"), std::string("kernel"),
             std::string("services"), std::string("pstore"), std::string("task")})
            scene.item(log_title(source), msg::read_only_select_to_browse, [logs, source] { logs(source); }, "report");
    } else if (page == "LOG_VIEW") {
        draw_logs(scene);
    } else if (page == "POWER") {
        scene.title(msg::power_startup, msg::review_the_consequences_before_rebooting_or_shutting_down_cancel);
        for (const auto* target : {"normal", "bootloader", "recovery", "poweroff"}) {
            const std::string key = "power:" + std::string(target);
            const auto operation = request(backend, key,en);
            scene.item(operation.name, std::string(target) == "normal" ?
                msg::clears_the_recovery_boot_request_then_restarts : msg::ends_this_recovery_session,
                [detail, key] { detail(key); }, "power", channel_reason(backend).empty());
        }
        scene.info({{msg::current_task, channel_reason(backend).empty() ? msg::no_known_conflicting_task : msg::action_unavailable_2},
            {msg::reboot_normally, msg::no_reboot_if_misc_readback_fails}, {msg::long_press_power, msg::handled_by_the_kernel}});
    } else if (page == "BOOT_MODE") {
        scene.title(msg::next_boot_mode, msg::changes_only_the_next_boot_mode_does_not_restart);
        scene.item(msg::boot_normally_next_time, msg::clears_the_recovery_boot_request, [detail] { detail("normal"); }, "power");
        scene.item(msg::boot_to_recovery_next_time, msg::sets_a_recovery_boot_request, [detail] { detail("recovery"); }, "power");
        scene.info({{msg::target, "misc / BCB"}, {msg::verification, msg::readback_after_write}, {msg::on_failure, msg::stop_without_rebooting}});
    } else if (page == "DISPLAY" || page == "LAYER_EDIT") {
        scene.title(msg::display_depth, msg::preview_changes_live_applied_changes_last_only_for_this);
        scene.item(msg::stereo_depth_mode, draft.zero ? msg::no_disparity_identical_views : msg::layered_depth_separate_offsets,
            [this] { draft.zero = !draft.zero; }, "stereo");
        constexpr std::array labels{msg::background_recession, msg::button_elevation, msg::text_elevation, msg::icon_elevation};
        constexpr std::array bounds{16, 12, 8, 8};
        const std::array values{draft.bg, draft.button, draft.text, draft.icon};
        for (int i = 0; i < 4; ++i)
            scene.item(labels[i], formatted(msg::arg0_px_range_0_arg1_step_2,en,{std::to_string(values[i]),std::to_string(bounds[i])}),
                [this, i] { editing = i; page = "LAYER_EDIT"; }, "stereo", !draft.zero, i);
        scene.item(msg::reset_stereo_depth, msg::resets_depth_only_not_the_system, [this] { draft = Depth{}; }, "refresh");
        scene.button(msg::apply, [this] { depth = draft; editing = -1; page = "DISPLAY"; back(); },
            36, 1030, 408, "ok");
        scene.button(msg::cancel, [this] { draft = depth; editing = -1; page = "DISPLAY"; back(); },
            460, 1030, 408);
        const int bg = draft.zero ? 0 : -draft.bg;
        const int buttons = draft.zero ? 0 : draft.button;
        const int text = draft.zero ? 0 : draft.button + draft.text;
        const int icons = draft.zero ? 0 : draft.button + draft.text + draft.icon;
        const auto signed_value = [](int value) {
            return (value > 0 ? "+" : "") + std::to_string(value);
        };
        scene.info({{msg::background_buttons, formatted(msg::arg0_arg1_px,en,{signed_value(bg),signed_value(buttons)})},
            {msg::text_icons, formatted(msg::arg0_arg1_px,en,{signed_value(text),signed_value(icons)})},
            {msg::controls, editing >= 0 ? msg::volume_adjusts_release_power_to_finish : msg::release_power_to_edit_the_selected_item},
            {msg::scope_2, msg::this_session_only_no_writes_to_persist}});
        scene.text(920, 800, 456, 202,
            msg::total_disparity_d_xl_xr_left_eye_d_2, muted, 25);
    } else if (page == "EXIT") {
        const auto operation = request(backend, argument,en);
        scene.title(argument == "power:poweroff" ? msg::shutting_down : msg::leaving_recovery,
            msg::waiting_for_the_device_to_perform_the_power_action);
        scene.message(operation.name, operation.effect, "info", 360, 306);
        scene.text(140, 740, 1180, 160,
            formatted(msg::arg0_task_status_is_no_longer_shown_after_recovery,en,{tr(observed(backend.task.detail))}), muted);
    } else {
        scene.title(msg::page_not_found, msg::no_action_was_run);
        scene.button(msg::back_to_home_2, home);
    }

    scene.center_buttons();
    if (page != "LOG_VIEW" && !report_detail())
        scene.footer(page == "BOOT" || page == "EXIT" ? msg::updates_automatically :
            page == "DISPLAY" || page == "LAYER_EDIT" ? msg::live_preview_not_applied : "");
    if (!actions.empty() && page != "LOG_VIEW") {
        const int count = static_cast<int>(actions.size());
        const int current = std::clamp(focus, 0, count - 1);
        int next = current;
        if (!actions[current]) {
            const auto enabled = std::find_if(actions.begin(), actions.end(),
                [](const std::function<void()>& action) { return static_cast<bool>(action); });
            next = enabled == actions.end() ? 0 : static_cast<int>(enabled - actions.begin());
        }
        if (next != focus) {
            focus = next;
            compose();
            return;
        }
    }
    overlay_begin=nodes.size();
    if (!choice.empty()) {
        Scene popup{*this};
        const double w=736,x=120,h=96+choice_options.size()*64;
        const double row_y=332+std::max(0,focus-1)*112;
        const double y=row_y+100+h<=1136?row_y+100:std::max(244.,row_y-h-8);
        popup.box(x,y,w,h,window,Layer::button,rule,2);
        nodes.back().floating_shadow=true;
        for(int i=0;i<=static_cast<int>(choice_options.size());++i){
            const bool cancel=i==static_cast<int>(choice_options.size()),selected=i==choice_focus;
            const double yy=y+8+i*64+(cancel?16:0);
            if(cancel) popup.box(x+16,yy-8,w-32,2,rule,Layer::button,rule,0,0);
            if(selected) popup.box(x+8,yy,w-16,64,power_held?selection:Color{51/255.,75/255.,92/255.},
                                    Layer::button,selection,2);
            popup.text(x+24,yy,w-48,64,cancel?msg::cancel:choice_options[i].second,
                       selected&&power_held?selected_ink:ink,29.333,false,false,false,true);
        }
        for(auto i=overlay_begin;i<nodes.size();++i)nodes[i].extra_depth=12;
    } else if (dialog) {
        Scene overlay{*this, 0, 0};
        overlay.box(0, 0, eye_size, eye_size, Color{0, 0, 0, 0.44},
            Layer::background, rule, 0, 0);
        const auto dialog_begin = nodes.size();
        overlay.x = 560; overlay.y = 776;
        overlay.box(0, 0, 1040, 608, window, Layer::button, rule, 2);
        nodes.back().floating_shadow=true;
        const std::string key = dialog_action == "bootmode" ? dialog_argument :
            dialog_action == "probe" ? "probe" : dialog_action + ":" + dialog_argument;
        const auto operation = deadline_confirmation?Request{}:request(backend, key,en);
        const bool read_only=dialog_action=="probe"||dialog_action=="hash"||dialog_action=="stockhash";
        const bool destructive=dialog_action=="format"||dialog_action=="wipe"||key=="install:all";
        overlay.icon(36, 30,read_only?"info":destructive?"delete":"warning",
                     read_only?ink:destructive?negative:neutral);
        overlay.text(104, 26, 898, 104, dialog_title, ink, 36, true);
        overlay.text(40, 132, 960, 250, dialog_body);
        overlay.text(40, 408, 960, 76,msg::cancel_is_selected_by_default_review_the_consequences_then, muted,25);
        overlay.box(36,498,968,2,rule,Layer::button,rule,0,0);
        for (int i = 0; i < 2; ++i) {
            const bool chosen = dialog_focus == i;
            overlay.box(164 + i * 430, 520, 408, 64,
                overlay.highlight(chosen,window), Layer::button, chosen ? selection : rule, 2);
            overlay.text(174 + i * 430, 520, 388, 64, i ? msg::cancel :
                deadline_confirmation?msg::save_and_reboot:operation.label,
                chosen ? selected_ink : ink, 25, false, false, true, true);
        }
        for (auto i = dialog_begin; i < nodes.size(); ++i)
            nodes[i].extra_depth = 12;
    }
    if(backend.install_preview){
        // Visible on every fixture page, including the language picker and home.
        const bool deck=progress_ui::has_log_deck(*this);
        const double px=deck?654:398,py=deck?96:1276,w=deck?390:644,h=deck?30:40;
        scene.box(px,py,w,h,window,Layer::background,rule,0,0);
        scene.text(px,py,w,h,msg::preview_no_partition_writes,neutral,20,false,false,true,true);
    }
    status_begin=nodes.size();
    scene.status();
}

} // namespace p4ui
