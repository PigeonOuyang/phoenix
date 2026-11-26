// SPDX-License-Identifier: Unlicense
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace p4ui {
namespace msg {
#define P4_MSG(id, text) inline constexpr char id[] = "ui." #id;
#include "locales/en.inc"
#undef P4_MSG
}
namespace locale_detail {
struct Entry { std::string_view key, text; };
#define P4_MSG(id, text) Entry{msg::id, text},
inline constexpr auto english = std::to_array<Entry>({
#include "locales/en.inc"
});
inline constexpr auto chinese = std::to_array<Entry>({
#include "locales/zh-CN.inc"
});
#undef P4_MSG

#define P4_LOG(id, text) Entry{"log." #id, text},
inline constexpr auto log_english = std::to_array<Entry>({
#include "locales/log.en.inc"
});
inline constexpr auto log_chinese = std::to_array<Entry>({
#include "locales/log.zh-CN.inc"
});
#undef P4_LOG

constexpr unsigned arguments(std::string_view text) {
    unsigned result=0;
    for (std::size_t i=0; i+2<text.size(); ++i)
        if (text[i]=='{' && text[i+2]=='}' && text[i+1]>='0' && text[i+1]<='9')
            result |= 1u << (text[i+1]-'0');
    return result;
}

constexpr bool valid_catalog(std::span<const Entry> first,std::span<const Entry> second) {
    if(first.size()!=second.size())return false;
    for (std::size_t i=0;i<first.size();++i) {
        if (first[i].key!=second[i].key || first[i].text.empty() || second[i].text.empty() ||
            arguments(first[i].text)!=arguments(second[i].text) ||
            (i && first[i-1].key>=first[i].key)) return false;
    }
    return true;
}
static_assert(valid_catalog(english,chinese) && valid_catalog(log_english,log_chinese),
              "Recovery locales must supply identical, sorted keys and format arguments");
}

// Foreign/raw values can pass unchanged. Never look up messages by translated text.
[[nodiscard]] constexpr std::string_view localized(std::string_view key, bool english) noexcept {
    if (key.starts_with("log.")) {
        const auto& logs = english ? locale_detail::log_english : locale_detail::log_chinese;
        const auto it = std::ranges::lower_bound(logs, key, {}, &locale_detail::Entry::key);
        return it != logs.end() && it->key == key ? it->text : key;
    }
    if (!key.starts_with("ui.")) return key;
    const auto& entries = english ? locale_detail::english : locale_detail::chinese;
    const auto it = std::ranges::lower_bound(entries, key, {}, &locale_detail::Entry::key);
    return it != entries.end() && it->key == key ? it->text : key;
}

namespace progress_ui {
struct ProgressWords {
    std::string_view id, full, short_label, compact{};
};

inline constexpr ProgressWords stage_words[] = {
    {"check", msg::progress_stage_check_label, msg::progress_stage_check_short},
    {"build-data", msg::progress_stage_build_data_label, msg::progress_stage_build_data_short, msg::progress_stage_build_data_compact},
    {"build", msg::progress_stage_build_label, msg::progress_stage_build_short},
    {"boot", msg::progress_stage_boot_label, msg::progress_stage_boot_short, msg::progress_stage_boot_compact},
    {"super", msg::progress_stage_super_label, msg::progress_stage_super_short, msg::progress_stage_super_compact},
    {"data", msg::progress_stage_data_label, msg::progress_stage_data_short},
    {"keys", msg::progress_stage_keys_label, msg::progress_stage_keys_short},
    {"assets", msg::progress_stage_assets_label, msg::progress_stage_assets_short},
    {"finish", msg::progress_stage_finish_label, msg::progress_stage_finish_short, msg::progress_stage_finish_compact},
};

inline constexpr ProgressWords child_words[] = {
    {"check", msg::progress_substep_check_label, msg::progress_substep_check_short},
    {"label", msg::progress_substep_label_label, msg::progress_substep_label_short},
    {"format", msg::progress_substep_format_label, msg::progress_substep_format_short},
    {"settle", msg::progress_substep_settle_label, msg::progress_substep_settle_short},
    {"label-ok", msg::progress_substep_label_ok_label, msg::progress_substep_label_ok_short},
    {"keep", msg::progress_substep_keep_label, msg::progress_substep_keep_short},
    {"inspect", msg::progress_substep_inspect_label, msg::progress_substep_inspect_short},
    {"existing-hash", msg::progress_substep_existing_hash_label, msg::progress_substep_existing_hash_short},
    {"no-write", msg::progress_substep_no_write_label, msg::progress_substep_no_write_short},
    {"write", msg::progress_substep_write_label, msg::progress_substep_write_short},
    {"flush", msg::progress_substep_flush_label, msg::progress_substep_flush_short},
    {"read", msg::progress_substep_read_label, msg::progress_substep_read_short},
    {"digest", msg::progress_substep_digest_label, msg::progress_substep_digest_short},
    {"keys", msg::progress_substep_keys_label, msg::progress_substep_keys_short},
    {"keys-ok", msg::progress_substep_keys_ok_label, msg::progress_substep_keys_ok_short},
    {"manifest", msg::progress_substep_manifest_label, msg::progress_substep_manifest_short},
    {"resources-keep", msg::progress_substep_resources_keep_label, msg::progress_substep_resources_keep_short},
    {"receive-assets", msg::progress_substep_receive_assets_label, msg::progress_substep_receive_assets_short},
    {"archive-hash", msg::progress_substep_archive_hash_label, msg::progress_substep_archive_hash_short},
    {"extract-assets", msg::progress_substep_extract_assets_label, msg::progress_substep_extract_assets_short},
    {"verify-files", msg::progress_substep_verify_files_label, msg::progress_substep_verify_files_short},
    {"activate-assets", msg::progress_substep_activate_assets_label, msg::progress_substep_activate_assets_short},
    {"retain", msg::progress_substep_retain_label, msg::progress_substep_retain_short},
    {"misc-write", msg::progress_substep_misc_write_label, msg::progress_substep_misc_write_short},
    {"misc-check", msg::progress_substep_misc_check_label, msg::progress_substep_misc_check_short},
    {"handoff", msg::progress_substep_handoff_label, msg::progress_substep_handoff_short},
    {"source", msg::progress_substep_source_label, msg::progress_substep_source_short},
    {"host-env", msg::progress_substep_host_env_label, msg::progress_substep_host_env_short},
    {"holo", msg::progress_substep_holo_label, msg::progress_substep_holo_short},
    {"holo-verify", msg::progress_substep_holo_verify_label, msg::progress_substep_holo_verify_short},
    {"holo-cache", msg::progress_substep_holo_cache_label, msg::progress_substep_holo_cache_short},
    {"holo-stall", msg::progress_substep_holo_stall_label, msg::progress_substep_holo_stall_short},
    {"download-stop", msg::progress_substep_download_stop_label, msg::progress_substep_download_stop_short},
    {"repos", msg::progress_substep_repos_label, msg::progress_substep_repos_short},
    {"package-fetch", msg::progress_substep_package_fetch_label, msg::progress_substep_package_fetch_short},
    {"package-cache", msg::progress_substep_package_cache_label, msg::progress_substep_package_cache_short},
    {"package-install", msg::progress_substep_package_install_label, msg::progress_substep_package_install_short},
    {"image-root", msg::progress_substep_image_root_label, msg::progress_substep_image_root_short},
    {"project-pkgs", msg::progress_substep_project_pkgs_label, msg::progress_substep_project_pkgs_short},
    {"elf", msg::progress_substep_elf_label, msg::progress_substep_elf_short},
    {"adapt", msg::progress_substep_adapt_label, msg::progress_substep_adapt_short},
    {"archive", msg::progress_substep_archive_label, msg::progress_substep_archive_short},
    {"filesystem", msg::progress_substep_filesystem_label, msg::progress_substep_filesystem_short},
    {"fsck", msg::progress_substep_fsck_label, msg::progress_substep_fsck_short},
    {"image-hash", msg::progress_substep_image_hash_label, msg::progress_substep_image_hash_short},
    {"compress", msg::progress_substep_compress_label, msg::progress_substep_compress_short},
    {"record", msg::progress_substep_record_label, msg::progress_substep_record_short},
};

inline const ProgressWords* progress_words(std::span<const ProgressWords> entries,std::string_view id) {
    const auto match=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return item.id==id;});
    return match==entries.end()?nullptr:&*match;
}
} // namespace progress_ui
} // namespace p4ui
