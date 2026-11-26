// SPDX-License-Identifier: Unlicense
#pragma once

#include "locale.hpp"
#include <initializer_list>
#include <string>

namespace p4ui::install_log {
// Presentation aliases for the English text emitted by the installers. These
// do not change the wire format, log file, or unknown messages from other tools.
struct Rule { std::string_view source, key; };
inline constexpr Rule messages[] = {
    {"Checking inputs and device", msg::check_inputs_and_device},
    {"Inputs, Recovery, display and battery checks passed", msg::inputs_recovery_display_and_battery_checks_passed},
    {"Preparing user data for the headset build", "log.build_data"},
    {"Build data is ready", "log.build_data_ready"},
    {"Building on the headset", "log.headset_build"},
    {"Headset build returned an image record", "log.headset_record"},
    {"Preparing or checking user data", "log.prepare_data"},
    {"pico4-data initialized or verified for reuse", "log.data_ready"},
    {"Writing public keys and installation ID", "log.keys"},
    {"Device verified the public keys and installation ID", "log.keys_verified"},
    {"Checking or writing backed-up system assets", "log.assets"},
    {"Device verified the system asset manifest", msg::device_verified_the_system_asset_manifest},
    {"Checking that Recovery remains selected", "log.check_recovery"},
    {"Readback confirms Recovery remains selected; SteamOS did not start", msg::readback_confirms_recovery_remains_selected_steamos_did_not_start},
    {"Clearing the Recovery request and reading back misc", "log.clear_recovery"},
    {"Readback confirms normal boot in misc", msg::readback_confirms_normal_boot_in_misc},
    {"Requested maintenance steps completed", "log.maintenance_complete"},
    {"Host workflow failed; see the original host and device logs", "log.host_failed"},
    {"Host workflow disconnected; see the original log", "log.host_disconnected"},
    {"Backend command completed", "log.backend_complete"},
    {"Backend command running", "log.backend_running"},
    {"Backend command running; progress unknown", "log.backend_unknown"},
    {"Backend command failed; see the original task log", "log.backend_failed"},
    {"Command failed or read-only restoration could not be verified; see the original log", "log.readonly_failed"},
    {"Leaving Recovery; next boot has not been verified", "log.handoff"},
    {"Requested backup and battery checks passed", "log.restore_checked"},
    {"System restore was not requested", "log.no_system_restore"},
    {"Data preparation was not requested", "log.no_data_preparation"},
    {"About to enter host fastboot; this UI cannot observe flashing", "log.host_fastboot"},
    {"Recovery restore was not requested", "log.no_recovery_restore"},
    {"SteamOS installation", "log.install_title"},
    {"System installation", "log.install_title"},
    {"Factory restore", msg::restore_the_factory_system},
    {"Collect hardware inventory", msg::collect_hardware_info},
    {"Set boot selection", msg::next_boot_mode},
    {"Power and startup", msg::power_startup},
    {"Format data partition", msg::format_for_linux_data},
    {"Verify local factory assets", msg::verify_asset_copy},
    {"On-device build", msg::on_device_build},
    {"Restore and verify boot", "log.restore_boot"},
    {"Restore and verify super", "log.restore_super"},
    {"Restore Recovery through host fastboot", "log.restore_recovery"},
    {"Check backup and device", "log.restore_inspect"},
    {"Prepare metadata", "log.restore_metadata"},
    {"Configure public keys", msg::install_public_keys_install_id},
    {"Install local factory assets", msg::update_device_system_assets},
    {"Compute partition SHA-256", msg::compute_partition_sha_256},
    {"Prepare userdata", msg::prepare_user_data},
    {"Select normal boot", msg::select_normal_boot},
    {"Download Holo Core", "log.holo"},
    {"Check userdata label", "log.userdata_label"},
    {"Create ext4", "log.create_ext4"},
    {"Sync and restore read-only protection", "log.sync_readonly"},
    {"Verify userdata label", "log.verify_label"},
    {"Check image and partition", "log.inspect_image"},
    {"Read back existing image", "log.existing_image"},
    {"Write image", "log.write_image"},
    {":: Running post-transaction hooks...", "log.pacman_hooks"},
    {":: Processing package changes...", "log.pacman_changes"},
    {":: Proceed with installation? [Y/n]", "log.pacman_proceed"},
    {"Skipped: Running in chroot.", "log.pacman_chroot"},
    {"assembling the image root", "log.image_root"},
    {"the image's project packages", "log.project_packages"},
    {"checking ELF dependencies of Frame and project packages", "log.elf"},
    {"checking ELF dependencies", "log.elf"},
    {"applying the PICO 4 adaptation layer", "log.adapt"},
    {"writing archive and manifest", "log.archive"},
    {"built", "log.headset_built"},
    {"Sync disk", "log.sync_disk"},
    {"Read back for SHA-256", "log.read_for_hash"},
    {"Confirm SHA-256", "log.confirm_digest"},
    {"Receive and extract build source", "log.source"},
    {"Prepare build environment", "log.build_environment"},
    {"Verify and extract Holo Core", "log.holo_verify"},
    {"Prepare repositories", "log.repositories"},
    {"Download missing packages", "log.download_packages"},
    {"Install base packages", "log.base_packages"},
    {"Assemble image root", "log.image_root"},
    {"Install project packages", "log.project_packages"},
    {"Check ELF dependencies", "log.elf"},
    {"Apply PICO 4 adaptation", "log.adapt"},
    {"Write userspace archive and manifest", "log.archive"},
    {"Create ext4 image", "log.filesystem"},
    {"Check ext4", "log.fsck"},
    {"Calculate image SHA-256", "log.image_hash"},
    {"Compress system image", "log.compress"},
    {"Receive image record and manifest", "log.record"},
    {"Check asset manifest", "log.asset_manifest"},
    {"Receive asset archive", "log.asset_receive"},
    {"Check archive SHA-256", "log.asset_hash"},
    {"Extract verified archive", "log.asset_extract"},
    {"Verify asset files", "log.asset_files"},
    {"Commit and confirm assets", "log.asset_confirm"},
    {"Confirm Recovery boot selection", "log.check_recovery"},
    {"Set normal boot selection", "log.misc_write"},
    {"Read back boot selection", "log.misc_read"},
    {"Leave Recovery; first boot unverified", "log.handoff"},
};

// One dynamic field per rule. Values (names, hashes, URLs, counts and paths)
// are copied only into a matching English diagnostic envelope.
inline constexpr Rule patterns[] = {
    {"Checking or writing {0}; waiting for device readback", msg::checking_or_writing_arg0_waiting_for_device_readback},
    {"Device readback verified {0} by SHA-256", msg::device_readback_verified_arg0_by_sha_256},
    {"Restoring {0}; waiting for device readback", "log.restore_partition"},
    {"Device readback verified the existing zeroed range of {0}", "log.restore_range"},
    {"Device readback verified {0}", "log.readback_verified"},
    {"Preparing {0}; zeroing and reading back only the existing range", "log.prepare_range"},
    {"installing {0}...", "log.pacman_install"},
    {"upgrading {0}...", "log.pacman_upgrade"},
    {"reinstalling {0}...", "log.pacman_reinstall"},
    {"checking {0}...", "log.pacman_check"},
    {"loading {0}...", "log.pacman_load"},
    {"resolving dependencies...", "log.pacman_resolve"},
    {"looking for conflicting packages...", "log.pacman_conflicts"},
    {"Synchronizing package databases...", "log.pacman_sync"},
    {"Retrieving packages...", "log.pacman_download"},
    {"downloading Holo Core aarch64 {0}", "log.holo_release"},
    {"making the build host: Holo Core aarch64 {0}", "log.holo_host"},
    {"writing {0} bytes to boot", "log.write_boot"},
    {"pico4-install: writing {0}", "log.write_failure"},
    {"pico4-install: readback of {0}", "log.readback_failure"},
    {"pico4-install: could not read back {0}", "log.readback_unreadable"},
    {"pico4-install: read-only restoration of {0}", "log.readonly_partition_failed"},
    {"pico4-install: {0}", "log.install_error"},
    {"error: {0}", "log.error"},
    {"warning: {0}", "log.warning"},
    {"install: {0}", "log.install_info"},
    {"written boot {0}", "log.written_boot"},
    {"written super {0}", "log.written_super"},
    {"bootmode: {0}", msg::boot_mode_arg0},
    {"Partition SHA-256: {0}", "log.partition_hash"},
    {"Read-only mount status: {0}", msg::current_mount_arg0},
    {"Optional dependencies for {0}", "log.pacman_optional"},
    {"checking {0}", "log.pacman_check"},
    {"Total Installed Size:  {0}", "log.pacman_size"},
    {"Net Upgrade Size:      {0}", "log.pacman_upgrade_size"},
    {"package lock: {0} packages as tested", "log.package_lock"},
    {"done: {0}", "log.output_artifact"},
};

inline std::string apply_field(std::string_view text,std::string_view field) {
    const auto at=text.find("{0}");
    if(at==text.npos)return std::string(text);
    std::string out(text.substr(0,at));
    out.append(field);
    out.append(text.substr(at+3));
    return out;
}

inline std::string message(std::string_view value,bool english) {
    if (const auto text=localized(value,english);text!=value) return std::string(text);
    if (english) return std::string(value);
    const auto trimmed=value.find_last_not_of(" \r\t");
    const auto body=value.substr(0,trimmed==value.npos?0:trimmed+1);
    for(const auto& rule:messages)
        if(rule.source==body)return std::string(localized(rule.key,false));
    for(const auto& rule:patterns){
        const auto at=rule.source.find("{0}");
        if(at==rule.source.npos){
            if(body==rule.source)return std::string(localized(rule.key,false));
            continue;
        }
        const auto head=rule.source.substr(0,at),tail=rule.source.substr(at+3);
        if(body.size()>head.size()+tail.size()&&body.starts_with(head)&&body.ends_with(tail))
            return apply_field(localized(rule.key,false),body.substr(head.size(),body.size()-head.size()-tail.size()));
    }
    return std::string(value);
}

inline std::string line(std::string_view value,bool english) {
    if(english)return std::string(value);
    auto body=value;
    if (body.starts_with(">> ")) {
        const auto at=body.starts_with(">> [")?body.find("] "):body.npos;
        body.remove_prefix(at==body.npos?3:at+2);
    } else if (body.starts_with("progress ")) {
        const auto at=body.find(": ");
        if(at!=body.npos)body.remove_prefix(at+2);
    } else if (body.starts_with('(')) {
        const auto at=body.find(") ");
        if(at!=body.npos){
            const auto count=body.substr(1,at-1);
            if(count.find_first_not_of("0123456789 /")==count.npos)body.remove_prefix(at+2);
        }
    }
    const auto first=body.find_first_not_of(" \t");
    if(first!=body.npos)body.remove_prefix(first);
    const auto text=message(body,false);
    if(text==body)return std::string(value);
    return std::string(value.substr(0,value.size()-body.size()))+text;
}
}
