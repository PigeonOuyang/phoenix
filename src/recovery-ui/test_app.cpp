// SPDX-License-Identifier: Unlicense
// Host model tests. Fixture services never execute commands or access partitions.
#include "ui.hpp"
#include "locale.hpp"
#include "log_layout.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace p4ui;
namespace {
int backend_calls=0;
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
constexpr int up = 115, down = 114, power = 116;
Preferences saved_choice(const std::string& storage = "data") {
 Preferences p; p.language_selected = true; p.durable = true; p.storage = storage; return p;
}
Preferences loaded_preferences = saved_choice();
bool fixture_has_system=true;
std::map<std::string, std::string> fixture_reads;
std::map<std::string, std::vector<std::string>> fixture_logs;
std::atomic<int> preference_calls{0};
struct SaveReply {
 Preferences preferences;
 bool saved = true, throws = false;
};
struct PendingSave {
 std::promise<void> started;
 std::future<void> entered = started.get_future();
 std::promise<SaveReply> completion;
 std::shared_future<SaveReply> reply = completion.get_future().share();
 std::string locale;
 std::thread::id thread;
};
std::mutex save_mutex;
std::shared_ptr<PendingSave> pending_save;
struct SaveAttempt {
 std::shared_ptr<PendingSave> state = std::make_shared<PendingSave>();
 bool released = false;
 SaveAttempt() {
  std::lock_guard lock(save_mutex);
  check(!pending_save, "only one fixture save at a time");
  pending_save = state;
 }
 ~SaveAttempt() {
  // Release on assertion failure before App's future destructor waits for the worker.
  if (!released) state->completion.set_value({Preferences{}, false, false});
  std::lock_guard lock(save_mutex);
  pending_save.reset();
 }
 void wait_started() {
  check(state->entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "save worker started");
  check(state->locale == "zh-CN", "physical activation saves the selected locale");
  check(state->thread != std::this_thread::get_id(), "save runs off the UI thread");
 }
 void finish(SaveReply reply) {
  state->completion.set_value(std::move(reply));
  released = true;
 }
};
bool has_text(const App& app, const std::string& value) {
 return std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
  return n.kind == Kind::text && n.value.find(value) != std::string::npos;
 });
}
void finish_save(App& app) {
 check(app.language_save.valid(), "save has an outstanding future");
 check(app.language_save.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
       "save completion arrives");
 app.compose();
 check(!app.language_save.valid(), "completion is consumed exactly once");
}
int geometry_checks = 0;
void geometry(const App& app) {
 const double x = 360, y = 440, width = 1440;
 const auto status = std::count_if(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
  return n.kind == Kind::box && n.x == x && n.y == y && n.w == width && n.h == 48;
 });
 check(status == 1, "exactly one global status bar on every scene");
 check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
  return n.kind == Kind::box && n.x == x && n.y == y &&
         n.w == width && n.h == 1280 && n.border_width == 0 && n.radius == 0;
 }), "page background keeps its dimensions without an outer frame");
 const auto& d = app.page == "DISPLAY" || app.page == "LAYER_EDIT" ? app.draft : app.depth;
 const std::array<int, 4> depths{-d.bg, d.button, d.button + d.text, d.button + d.text + d.icon};
 bool dialog_foreground = false;
 for (const auto& n : app.nodes) {
  check(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.w) &&
        std::isfinite(n.h) && std::isfinite(n.size) && std::isfinite(n.extra_depth),
        "finite scene geometry");
  check(n.w >= 0 && n.h >= 0 && n.size > 0, "nonnegative scene geometry");
  const bool partial_context=n.reader_row>=0&&n.y<eye_size&&n.y+n.h>0;
  check((n.y >= 0 && n.y + n.h <= eye_size)||partial_context,
        "nodes fit one eye or are clipped reader context at the canvas boundary");
  const bool scrim = app.dialog && n.kind == Kind::box &&
                    n.x == 0 && n.y == 0 && n.w == eye_size && n.h == eye_size;
  if (scrim) dialog_foreground = true;
  if (!scrim) {
   for (const int eye : {-1, 1}) {
    const double shift = d.zero ? 0 : eye * (depths[static_cast<int>(n.layer)] + n.extra_depth) / 2.;
    check(n.x + shift >= 0 && n.x + n.w + shift <= eye_size,
          "nodes fit horizontally in both eyes after layer disparity");
   }
  }
  if (n.kind == Kind::box && n.layer == Layer::button && n.h == 92)
   check(n.w == 820, "original menu row dimensions");
  if (dialog_foreground && !scrim && n.extra_depth>0)
   check(n.y >= 660 && n.y + n.h <= 1500, "dialog foreground does not cover global status");
 }
 if (app.page == "CURRENT_TASK" || app.page == "RUNNING" || app.page == "SUCCESS" ||
     app.page == "ERROR" || app.page == "HASH" || app.page == "MOUNT")
  check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
   return n.kind == Kind::box && n.layer == Layer::button &&
          n.y == 1388 && n.h == 12;
  }), "task progress retains its original height");
 const auto clock = std::find_if(app.nodes.begin(), app.nodes.end(), [](const Node& n) {
  return n.kind == Kind::text && n.y == 440 && n.h == 48 && n.center;
 });
 check(clock != app.nodes.end() && clock->x + clock->w/2 == 1080 && clock->middle &&
       clock->ink_height == 24 && clock->cjk_bold,
       "time and date occupy the fixed middle status slot");
 for (const auto* name : {"bluetooth-off", "wifi-off", "ethernet-off"})
  check(std::count_if(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
   return n.kind == Kind::icon && n.value == name && n.y == 448 && (n.w == 32 || n.w == 36) &&
          n.x < 664 && n.color.r == muted.r;
  }) == 1, "network placeholder icons are disabled in the reserved left slot");
 check(std::any_of(app.nodes.begin(), app.nodes.end(), [](const Node& n) {
  return n.kind == Kind::icon && n.value.starts_with("ncm:") && n.x+n.w/2 == 392 && n.y == 448;
 }), "NCM occupies the far-left slot");
 const auto battery = std::find_if(app.nodes.begin(), app.nodes.end(), [](const Node& n) {
  return n.kind == Kind::icon && n.value.starts_with("battery:");
 });
 check(battery != app.nodes.end() && battery->x == 1624 && battery->w == 44 &&
       battery->ink_height == 26,
       "battery icon precedes the rightmost percentage");
 check(std::any_of(app.nodes.begin(), app.nodes.end(), [](const Node& n) {
  return n.kind == Kind::text && n.x == 1688 && n.w == 88 && n.right && n.ink_height == 24 &&
         n.previous_icon_gap == 8;
 }), "percentage occupies the slot to the right of the battery");
 for(const auto& n:app.nodes)
  if(n.kind==Kind::icon && n.y==448)
   check(n.ink_height>=26, "status icons retain visible size beside numeric labels");
for(const auto& [name,px]:std::array<std::pair<const char*,double>,2>{
       {{"upload",1436},{"download",1608}}})
  check(std::any_of(app.nodes.begin(), app.nodes.end(),[&](const Node& n) {
   return n.kind==Kind::icon && n.value==name && n.x==px && n.y==504 &&
          n.w==32 && n.h==32 && n.ink_height==28;
  }), "traffic directions retain size in the row below the battery");
 for(const auto& n:app.nodes) {
  const bool content=n.ink_height==0 &&
      (n.kind==Kind::text||n.kind==Kind::icon||(n.kind==Kind::box&&n.layer==Layer::button));
  if(!content||n.reader_row>=0||n.y+n.h<=496||n.y>=544)continue;
  for(const int eye:{-1,1}) {
   const double shift=d.zero?0:eye*(depths[static_cast<int>(n.layer)]+n.extra_depth)/2.;
   const double flow_left=1436+(d.zero?0:eye*depths[static_cast<int>(Layer::icon)]/2.);
   const double flow_right=1776+(d.zero?0:eye*depths[static_cast<int>(Layer::text)]/2.);
   check(n.x+n.w+shift<=flow_left||n.x+shift>=flow_right,
         "traffic row does not obscure page text or controls at any supported disparity");
  }
 }
 check(!has_text(app, "仅物理三键"), "redundant footer note is removed");
 if (app.page == "LOG_VIEW") {
  check(std::none_of(app.nodes.begin(), app.nodes.end(), [](const Node& n) {
   return n.kind == Kind::icon && (n.value == "up" || n.value == "down" || n.value == "power");
  }), "log view retains its simple layout without a navigation footer");
  constexpr auto viewport=reader_viewport(false);
  check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
   return n.reader_backdrop && n.x+n.w/2==1080 && n.y==viewport.top &&
          n.w==app.log_width.viewport && n.y+n.h==viewport.bottom();
  }), "log backdrop still covers the original full-brightness body");
 } else if (!app.report_detail()) {
  for (const auto& [name, px] : std::array<std::pair<const char*, double>, 3>{
       {{"up", 623}, {"down", 929}, {"power", 1235}}})
   check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
    return n.kind == Kind::icon && n.value == name && n.x == px && n.y == 1640;
   }), "bottom key hint group is centered and constrained");
 }
 geometry_checks++;
}
void host_install_arrival() {
 fixture_has_system=true;
 loaded_preferences=saved_choice("data");
 App app;
 check(app.page=="HOME"&&app.settings().locale=="zh-CN","start with the saved language on Home");
 auto& task=app.backend.task;
 task.id="restore";task.kind="restore";task.status="running";task.locked=true;
 check(!app.observe_install_task()&&app.page=="HOME","stock restore is not a new SteamOS install");
 task.id="pc";task.kind="host";
 check(app.observe_install_task()&&app.page=="LOG_VIEW"&&app.log_source=="install-live"&&
       app.follow_install_log&&app.settings().locale=="zh-CN","a fresh PC install opens the real log and preserves locale");
 app.back();
 check(!app.observe_install_task()&&app.page=="HOME","the same task never forces the user back to its log");
 task.id="headset";task.kind="headset";
 check(app.observe_install_task()&&app.page=="LOG_VIEW","a new headset build opens its real install log");
 loaded_preferences=Preferences{};
 App initial;
 initial.backend.task=task;
 check(initial.observe_install_task()&&initial.settings().locale=="en","no saved language uses English on live entry");
 loaded_preferences=Preferences{};
}

void first_language_saves() {
 fixture_has_system=false;
 std::vector<std::string> boot(30,"Fixture Recovery message");
 fixture_logs["/run/overpressure/boot.log"]=boot;
 fixture_logs["/run/overpressure/task.log"]={};
 loaded_preferences = Preferences{};
 loaded_preferences.error = "语言设置：resolve /dev/block/by-name/userdata: No such file or directory";
 App app;
 app.compose();
 check(app.page=="LOG_VIEW"&&app.log_source=="install-live"&&app.settings().locale=="en"&&app.follow_install_log,
       "an uninstalled headset without a language opens the English live log");
 check(has_text(app,"Installation has not started. Waiting for the host.")&&
       std::none_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::stripes;}),
       "idle installation shows no fabricated activity");
 check(app.log_top==app.log_layout.max_top(),"first entry follows the end of the boot log");
 fixture_logs["/run/overpressure/task.log"]={"Observed write start",
    "[running] Read back for SHA-256",
    "[done] Inputs, Recovery, display and battery checks passed",
    "[error] Third-party diagnostic E42"};
 app.backend.task.id="first-install-fixture";
 app.backend.task.kind="host";
 app.backend.task.stages={{"Check inputs","done",{}},{"Write boot","running",{}}};
 app.backend.task.locked=true;app.backend.task.status="running";
 app.compose();
 check(has_text(app,"Observed write start")&&app.log_top==app.log_layout.max_top()&&
       std::any_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::stripes;}),
       "device records advance the live log and activate the real stage");
 check(has_text(app,"Read back for SHA-256")&&has_text(app,"battery checks passed")&&
       has_text(app,"Third-party diagnostic E42")&&!has_text(app,"回读"),
       "English preserves installer and raw error text");
 app.backend.preferences.locale="zh-CN";
 app.compose();
 check(has_text(app,"回读并计算 SHA-256")&&has_text(app,"Third-party diagnostic E42"),
       "Chinese localizes known log lines and retains the original diagnostics");
 check(fixture_logs["/run/overpressure/task.log"][1]=="[running] Read back for SHA-256",
       "the display translation does not modify the stored line");
 app.backend.preferences.locale="en";
 app.compose();
 app.key(up);
 const auto prior_top=app.log_top;
 fixture_logs["/run/overpressure/task.log"].push_back("Still reading back");
 app.compose();
 check(!app.follow_install_log&&app.log_top==prior_top,"manual browsing is not pulled back to the tail");
 app.key(power);
 check(app.follow_install_log&&app.log_top==app.log_layout.max_top(),"Back focus resumes following the tail");
 app.key(power);
 check(app.page == "LANGUAGE" && app.actions.size() == 3, "choosing Home opens the two languages and link back to the log");
 check(has_text(app, "No Storage Selected") && has_text(app, "No settings storage is available"),
       "initial storage and preference diagnostics are translated");
 check(!has_text(app, "resolve ") && !has_text(app, "No such file"), "raw load error is not UI text");
 const int deferred = preference_calls;
 app.key(down);app.key(down);app.key(power);app.compose();
 check(app.uncommitted&&app.uncommitted->locale=="zh-CN"&&!app.language_save.valid()&&
       preference_calls==deferred,"language selection waits for a busy real install");
 app.backend.task.locked=false;app.animation_phase=2;
 {
  SaveAttempt attempt;
  const int calls = preference_calls;
  app.compose();
  attempt.wait_started();
  check(app.language_save.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
        "save remains genuinely asynchronous while pending");
  app.key(power); app.key(up); app.key(down); app.navigate("HOME"); app.back(); app.compose();
  check(preference_calls == calls + 1, "repeat physical activation cannot duplicate a pending save");
  check(app.page == "LANGUAGE" && app.stack.empty() && !app.backend.preferences.language_selected,
        "first entry cannot continue before durable save");
  check(!app.backend.preferences.durable, "pending UI does not claim success");
  geometry(app);
  attempt.finish({saved_choice("cache"), true, false});
  finish_save(app);
 }
 check(app.page == "HOME" && app.focus==8 && app.backend.task.stages.size()==2,
       "first successful save opens Home and retains the active task");
 check(app.backend.preferences.durable && app.backend.preferences.language_selected &&
       app.backend.preferences.storage == "cache" && app.notice.empty(), "successful save adopts verified preferences");
 app.key(power);
 check(app.page == "LOG_VIEW" && app.log_source=="install-live","Home can reopen the same live log");
 loaded_preferences = app.backend.preferences;
 App reentered;
 check(reentered.page=="LOG_VIEW"&&reentered.settings().locale=="zh-CN","uninstalled reentry follows logs in the saved language");
 reentered.key(power);
 check(reentered.page=="HOME","a saved language skips the picker");

 loaded_preferences = Preferences{};
 loaded_preferences.storage = "data";
 App retry;
 retry.key(power);retry.key(down);retry.key(down);
 retry.backend.task.id = "fixture"; retry.backend.task.status = "done";
 const std::string raw = "语言设置：sync preferences temporary: No space left on device";
 {
  SaveAttempt attempt;
  retry.key(power); attempt.wait_started();
  auto failed = saved_choice(); failed.durable = false; failed.error = raw;
  attempt.finish({failed, false, false}); finish_save(retry);
 }
 check(retry.page == "LANGUAGE" && retry.stack.empty() && !retry.backend.preferences.language_selected,
       "failed first save keeps the selection gate closed");
 check(retry.backend.preferences.error == raw && retry.notice.find(raw) == std::string::npos,
       "raw save detail is retained only as a diagnostic");
 check(has_text(retry, "尚未确认语言设置已保存") && has_text(retry, "存储空间") && has_text(retry, "数据分区"),
       "failed save has a translated primary message and actionable reason");
 {
  SaveAttempt attempt;
  retry.key(power); attempt.wait_started();
  attempt.finish({saved_choice(), true, false}); finish_save(retry);
 }
 check(retry.page == "HOME" && retry.focus==8 && retry.backend.preferences.error.empty(),
       "physical retry succeeds and opens Home");
 retry.key(power);
 check(retry.page == "LOG_VIEW" && retry.log_source=="install-live"&&retry.backend.task.status=="done",
       "log returns to the existing task without restarting it");

 loaded_preferences = Preferences{};
 loaded_preferences.storage = "cache";
 App exception;
 exception.key(power);exception.key(down);exception.key(down);
 {
  SaveAttempt attempt;
  exception.key(power); attempt.wait_started();
  attempt.finish({Preferences{}, false, true}); finish_save(exception);
 }
 check(exception.page == "LANGUAGE" && has_text(exception, "缓存分区") &&
       has_text(exception, "尚未确认语言设置已保存") && !has_text(exception, "fixture preference failure"),
       "worker exception is translated and remains retryable");
 {
  SaveAttempt attempt;
  exception.key(power); attempt.wait_started();
  attempt.finish({saved_choice("cache"), true, false}); finish_save(exception);
 }
 check(exception.page == "HOME", "retry after a thrown save completes");

 for (int invalid = 0; invalid < 7; ++invalid) {
  loaded_preferences = Preferences{};
  App rejected;
  rejected.key(power);rejected.key(down);rejected.key(down);
  SaveAttempt attempt;
  rejected.key(power); attempt.wait_started();
  auto result = saved_choice();
  if (invalid == 0) result.durable = false;
  if (invalid == 1) result.language_selected = false;
  if (invalid == 2) result.locale = "en-US";
  if (invalid == 3) result.storage = "none";
  if (invalid == 4) result.error = "installer failed";
  if (invalid == 5) result.storage = "unexpected";
  attempt.finish({result, invalid != 6, false}); finish_save(rejected);
  check(rejected.page == "LANGUAGE" && !rejected.backend.preferences.language_selected &&
        !rejected.backend.preferences.durable, "unverified or false-return save cannot bypass first entry");
 }
 fixture_has_system=true;
 loaded_preferences=Preferences{};
 App installed;
 check(installed.page=="LANGUAGE"&&!installed.first_install_entry,
       "an existing image with no language does not look like a fresh install");
 loaded_preferences = saved_choice();
 fixture_logs.erase("/run/overpressure/boot.log");
 fixture_logs.erase("/run/overpressure/task.log");
}

void live_log_notification() {
 const auto saved_preferences=loaded_preferences;
 const auto had_system=fixture_has_system;
 fixture_has_system=false;
 loaded_preferences={};
 const std::string boot="/run/overpressure/boot.log",task="/run/overpressure/task.log";
 fixture_logs[boot]={"baseline"};
 fixture_logs[task]={"first installer line"};
 App app;
 app.backend.task.kind="host";
 app.live_log_events=true;
 app.animation_phase=10;
 app.compose();
 check(has_text(app,"first installer line"),"initial live document loads existing output");
 fixture_logs[task].push_back("appended during the same second");
 app.animation_phase=10.02;
 app.compose();
 check(!has_text(app,"appended during the same second"),"animation alone does not reread the logs");
 app.live_log_dirty=true;
 app.compose();
 check(has_text(app,"appended during the same second"),"file notification refreshes without a one-second wait");
 fixture_logs[task]={"new task log after truncation"};
 app.live_log_dirty=true;
 app.compose();
 check(has_text(app,"new task log after truncation")&&!has_text(app,"first installer line"),
       "file notification drops stale rows after truncation");
 fixture_logs.erase(boot);
 fixture_logs.erase(task);
 fixture_has_system=had_system;
 loaded_preferences=saved_preferences;
}
void settings_language_saves() {
 App app;
 app.navigate("SETTINGS","language");app.compose();
 app.key(power);check(app.choice=="locale","Power opens a choice on the same page");
 app.key(down);
 {
  SaveAttempt attempt;
  app.key(power);
  check(attempt.state->entered.wait_for(std::chrono::seconds(5))==std::future_status::ready &&
        attempt.state->locale=="en","English save runs asynchronously");
  check(app.settings().locale=="en" && app.backend.preferences.locale=="zh-CN","English preview is immediate");
  auto failed = saved_choice(); failed.durable = false; failed.error = "unsafe preferences.json";
  attempt.finish({failed, false, false}); finish_save(app);
 }
 check(app.page=="SETTINGS"&&app.backend.preferences.locale=="zh-CN"&&app.uncommitted&&
       !app.save_error.empty(),"failed settings preserve verified preferences");
 app.focus=4;app.key(power);
 check(app.settings().locale=="zh-CN"&&!app.uncommitted,"undo restores the committed locale");
 {
  SaveAttempt attempt;
  app.focus=1;app.key(power);app.key(down);app.key(power);
  check(attempt.state->entered.wait_for(std::chrono::seconds(5))==std::future_status::ready,"retry starts");
  auto en=saved_choice();en.locale="en";
  attempt.finish({en,true,false});finish_save(app);
 }
 check(app.page=="SETTINGS"&&app.focus==1&&app.settings().locale=="en","save retains focused settings row");
 app.back();app.navigate("SETTINGS","keys");app.compose();
 app.key(power);app.key(down);
 {
  SaveAttempt attempt;
  app.key(power);
  check(attempt.state->entered.wait_for(std::chrono::seconds(5))==std::future_status::ready,"key save starts");
  check(app.settings().reverse,"reversed navigation applies immediately");
  app.key(up);
  check(app.focus==2,"reversed Volume+ advances focus");
  auto result=saved_choice();result.locale="en";result.reverse=true;
  attempt.finish({result,true,false});finish_save(app);
 }
 check(app.page=="SETTINGS"&&app.settings().reverse,"late completion preserves context");
}
void localization() {
 const std::array errors{
  std::pair{"BLKROSET ro failed", "恢复为只读"},
  std::pair{"保存超时，后台事务仍可能完成；请稍后重新读取", "后台可能仍在保存"},
  std::pair{"语言设置：flock install.lock: Resource temporarily unavailable", "正在使用设置存储"},
  std::pair{"语言设置尚在缓存，请保存以迁移至数据分区", "仍在缓存"},
  std::pair{"已保存的语言暂不支持，请重新选择", "不受支持"},
  std::pair{"unsupported preferences version", "版本"},
  std::pair{"read-only preferences readback differs", "未通过核对"},
  std::pair{"invalid preferences JSON", "无效"},
  std::pair{"userdata filesystem unknown; refusing cache fallback", "找不到可用"},
  std::pair{"filesystem needs recovery; no repair", "文件系统未通过安全检查"},
  std::pair{"unsafe preferences.json", "安全检查"},
  std::pair{"unmount preferences: Device or resource busy", "挂载或卸载"},
  std::pair{"sync preferences directory: Input/output error", "同步"},
  std::pair{"write preferences temporary: No space left on device", "存储空间"},
  std::pair{"unclassified internal diagnostic", "无法确认保存结果"}};
 for (const auto& [raw, translated] : errors) {
  loaded_preferences = Preferences{}; loaded_preferences.error = raw;
  App app; app.backend.preferences.locale="zh-CN"; app.compose();
  if(!has_text(app,translated))throw std::runtime_error(std::string("translated preference reason: ")+raw);
  check(has_text(app, "暂时无法读取已保存"), "translated preference heading");
  check(app.backend.preferences.error == raw, "presentation never destroys the original preference diagnostic");
  geometry(app);
 }
 loaded_preferences = saved_choice();
 App app;
 for (const auto& [storage, translated] : std::array{
       std::pair{"none", "尚未选定存储位置"}, std::pair{"data", "数据分区"},
       std::pair{"cache", "缓存分区"}, std::pair{"unexpected", "存储状态未知"}}) {
  app.backend.preferences.storage = storage; app.page = "LANGUAGE"; app.argument = "settings"; app.compose();
  check(has_text(app, translated), "all preference storage labels are localized");
 }
 for (const auto& [state, translated] : std::array{
       std::pair{"Charging", "充电中"}, std::pair{"Discharging", "放电中"},
       std::pair{"Full", "已充满"}, std::pair{"Not charging", "未充电"}, std::pair{"Unknown", "未知"}}) {
  app.page = "STATUS"; app.backend.telemetry.battery_status = state; app.compose();
  check(has_text(app, translated) && !has_text(app, state), "battery state text is localized");
  check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
   return n.kind == Kind::icon && n.value.ends_with(":" + std::string(state));
  }), "raw battery state remains available to the renderer icon");
 }
 for (const auto& [state, translated] : std::array{
       std::pair{"running", "进行中"}, std::pair{"waiting", "等待中"},
       std::pair{"done", "已完成"}, std::pair{"error", "失败"}, std::pair{"skipped", "已跳过"}}) {
  app.backend.task.id = "fixture"; app.backend.task.status = state; app.page = "HOME"; app.compose();
  check(has_text(app, translated) && !has_text(app, state), "home task state is localized");
 }
 app.backend.task = {};
 app.backend.partitions = {{"fixture", "/dev/fixture", "ext4", "", 512, true}};
 app.page = "PARTITION"; app.argument = "fixture"; app.compose();
 check(has_text(app, "512 字节") && !has_text(app, "bytes"), "byte unit is localized");
 app.page = "HOST"; app.compose(); check(has_text(app, "USB 链路") && !has_text(app, "USB link"), "host label localized");
 app.page = "DEADLINE"; app.compose();
 check(has_text(app, "连接时限") && has_text(app, "测试时限") && !has_text(app, "timeout"), "deadline labels localized");
}

void english_surface_uses_message_keys() {
 auto pref=saved_choice();pref.locale="en";loaded_preferences=pref;
 std::string_view en_probe=localized(msg::next_boot_mode,true);
 check(en_probe=="Next Boot Mode"&&localized("下次启动方式",true)=="下次启动方式",
       "the locale catalog looks up message keys only");
 App app;
 app.backend.preferences=pref;
 app.backend.telemetry.usb="usb0\tconfigured";
 app.backend.telemetry.uptime="125";
 app.backend.partitions={{"boot","/dev/sda1","ext4","",1024,true},
                         {"misc","/dev/sda2","raw","",1048576,true}};
 for (const auto& [page,arg] : std::array<std::pair<const char*,const char*>,16>{
      {{"HOME",""},{"STATUS",""},{"HOST",""},{"SERVICES",""},{"DEADLINE",""},{"STORAGE",""},
       {"PARTITION","boot"},{"TASK_DETAIL","hash:boot"},{"INPUTS","normal"},{"REVIEW","recovery"},
       {"DISPLAY",""},{"SETTINGS","traffic"},{"SETTINGS","keys"},{"SETTINGS","language"},
       {"REPORT",""},{"LOGS",""}}}) {
  app.page=page;app.argument=arg;app.compose();
  for(const auto& node:app.nodes){
   if(node.kind!=Kind::text||node.prewrapped)continue;
   for(std::size_t at=0;at+2<node.value.size();++at){
    const auto a=static_cast<unsigned char>(node.value[at]);
    const auto b=static_cast<unsigned char>(node.value[at+1]);
    if((a==0xe4&&b>=0xb8)||(a>=0xe5&&a<=0xe8)||(a==0xe9&&b<=0xbf))
     throw std::runtime_error(std::string("Chinese UI copy in ")+page+": "+node.value);
   }
   check(!node.value.starts_with("ui."),"named catalog IDs are not rendered");
  }
 }
 app.page="STATUS";app.compose();check(has_text(app,"2 h 5 min"),"localized uptime interpolation");
 app.page="HOST";app.compose();check(has_text(app,"usb0: Configured"),"localized USB state with raw controller ID");
 loaded_preferences=saved_choice();
}
void log_navigation() {
 App app;
 app.backend.task.id = "fixture"; app.backend.task.status = "running"; app.backend.task.locked = true;
 for (const auto& source : {"/run/overpressure/boot.log", "kernel", "services", "pstore", "task", "build"}) {
  app.page = std::string(source) == "build" ? "BUILD_MENU" : "LOGS";
  app.argument.clear(); app.focus = 2;
  const auto original = app.page;
  app.navigate("LOG_VIEW", source); app.compose(); geometry(app);
  check(app.log_back && app.focus == 0, "every log source enters with Back selected");
  app.key(power);
  check(app.page == original && app.focus == 2 && app.backend.task.locked,
        "default log Back restores the exact originating page and focus without cancelling task");
 }
 app.page = "LOGS"; app.focus = 3; app.navigate("LOG_VIEW", "task"); app.compose();
 app.key(down); check(!app.log_back && app.log_cursor == 0, "volume selects the first log entry");
 app.key(up); check(app.log_back, "volume at the first entry returns focus to Back");
 app.key(down);
 for (int i = 0; i < 99; ++i) app.key(down);
 check(app.log_cursor == 99 && !app.log_back && app.log_top > 0, "log selection scrolls to the final entry");
 geometry(app);
 const int top = app.log_top;
 app.key(down); check(app.log_back, "volume at the final entry returns focus to Back");
 app.key(power); check(app.page == "LOGS" && app.focus == 3, "boundary Back preserves menu selection");
 app.navigate("LOG_VIEW", "task"); app.compose();
 check(app.log_cursor == 99 && app.log_top == top && app.log_back, "reopening the same source preserves scroll but selects Back");
 app.key(up); app.key(power);
 check(app.page == "LOG_VIEW" && app.log_back, "power in log body selects Back without exiting");
 app.key(power); check(app.page == "LOGS", "next short power returns to original page");

 fixture_logs["/run/overpressure/report/dmesg.txt"] = {};
 app.navigate("LOG_VIEW", "kernel"); app.compose();
 check(app.log_cursor == 0 && app.log_top == 0 && app.log_back, "new source resets scroll");
 app.key(down); app.key(up); geometry(app);
 check(app.log_back && has_text(app, "暂无可读取的日志"), "empty log keeps Back reachable");
 app.key(power); check(app.page == "LOGS", "empty log Back restores source page");
 fixture_logs["/run/overpressure/task.log"] = {std::string(8000, 'x'), "final fixture diagnostic"};
 app.navigate("LOG_VIEW", "task"); app.compose(); app.key(down);
 app.key(down);
 check(app.log_cursor==1&&app.log_top==0,"inside the viewport, volume moves only the selection");
 const auto end=app.log_layout.entries[1].first_row;
 for(int expected=2;expected<=end;++expected){
  app.key(down);
  if(app.log_cursor!=expected)throw std::runtime_error("wrapped log rows must never be skipped");
  if(app.log_top!=std::max(0,expected-log_view_lines+1))
   throw std::runtime_error("log viewport must move by one row only when crossing its edge");
 }
 check(app.log_cursor==end&&!app.log_back&&app.log_top==app.log_layout.max_top(),
       "walk through a long record to the final entry without paging");
 geometry(app);
 const auto bottom_top=app.log_top;
 app.key(up);
 check(app.log_cursor==end-1&&app.log_top==bottom_top,"reversing inside the viewport keeps the page fixed");
 for(int i=1;i<log_view_lines-1;++i)app.key(up);
 check(app.log_cursor==bottom_top&&app.log_top==bottom_top,"top visible row remains fully selectable");
 app.key(up);
 check(app.log_cursor==bottom_top-1&&app.log_top==bottom_top-1,"crossing the upper edge scrolls exactly one row");
 while(!app.log_back)app.key(up);
 check(app.log_back&&app.log_cursor==0&&app.log_top==0,
       "oversized entry scrolls back to the beginning and Back");
 fixture_logs["/run/overpressure/task.log"] = {"shortened fixture"};
 app.log_cursor = 999; app.log_top = 999999; app.compose(); geometry(app);
 check(app.log_cursor == 0 && app.log_top == 0, "changed log contents clamp selection and viewport");
 app.key(power);
 fixture_logs.clear();
 check(app.backend.task.locked, "all log navigation leaves task execution intact");
}
void log_reflow() {
 App app;
 app.measure_log_glyph=[](std::string_view glyph){
  const double advance=glyph.size()==1?14:27;
  return LogGlyph{advance,advance};
 };
 const std::string typical=std::string(65,'a')+" 日志";
 const std::string long_item=std::string(8000,'a')+"\t日志/末尾";
 fixture_logs["/run/overpressure/task.log"]={typical,long_item,"最后一行"};
 app.navigate("LOG_VIEW","task");app.compose();
 check(app.log_layout.entries.front().lines.size()==1,"practical ASCII log line fits original viewport");
 check(parse_log_width("2040\n")==2040&&parse_log_width("2041")==log_width_default&&
       parse_log_width("1801")==log_width_default&&parse_log_width("1800junk")==log_width_default,
       "invalid width requests use default");
 app.key(down);
 for(int i=0;i<36;++i)app.key(down);
 check(app.log_top>1,"scrolled into long record to exercise reflow anchor");
 auto offset_at=[&](int row){
  const auto& entry=app.log_layout.entries[1];
  return static_cast<int>(entry.lines.at(row-entry.first_row).offset);
 };
 for(int w:{2040,1760,1970}){
  const auto anchor=offset_at(app.log_top),selection=offset_at(app.log_cursor);
  app.log_width.viewport=w;app.compose();geometry(app);
  check(!app.log_back&&std::abs(offset_at(app.log_cursor)-selection)<145,"reflow retains selected byte range");
  check(std::abs(offset_at(app.log_top)-anchor)<650,"reflow keeps nearby context while keeping selected line visible");
  std::string joined;
  for(const auto& line:app.log_layout.entries[1].lines)joined+=line.text;
  check(joined==std::string(8000,'a')+"    日志/末尾","wrap preserves log bytes and expands tabs");
 }
 fixture_logs.clear();
}
void report_reader() {
 App app;
 app.measure_log_glyph=[](std::string_view glyph) {
  const double advance=glyph.size()==1?14:27;
  return LogGlyph{advance,advance};
 };
 const std::string path="/run/overpressure/report/cpu.txt";
 const std::string long_mixed=std::string(80,'a')+" 读取结果";
 std::string result=long_mixed+"\n";
 for(int i=1;i<report_view_lines*2;++i)result+="fixture "+std::to_string(i)+"\n";
 fixture_reads[path]=result;
 fixture_reads["/proc/meminfo"]="MemTotal: 1234 kB\n";
 app.page="REPORT";app.focus=2;
 app.navigate("REPORT","cpu");app.compose();
 geometry(app);
 const auto command=[&] {
  for(const auto& n:app.nodes)if(n.prewrapped&&n.value=="$ cat cpu.txt /proc/meminfo")return n;
  throw std::runtime_error("report command is missing");
 };
 const auto first=command();
 check(app.report.lines.front().text==long_mixed,"mixed report fits one line at default width");
 check(app.report.lines.back().text=="MemTotal: 1234 kB","reads the configured live supplement");
 const auto viewport=reader_viewport(true);
 check(std::count_if(app.nodes.begin(),app.nodes.end(),[&](const Node& n){
  return n.reader_row>=0&&n.y>=viewport.top&&n.y+n.h<=viewport.bottom()&&n.h==log_line_px&&n.size==log_font_px;
 })==report_view_lines,"reports share log glyph and line dimensions and fill the view");
 const auto thumb=[&]{
  for(const auto& n:app.nodes)if(n.kind==Kind::arc&&n.border_width==6)return n.arc_begin;
  throw std::runtime_error("report scroll thumb is missing");
 };
 const auto start=thumb();
 fixture_reads[path]="updated source\n";
 app.key(up);
 check(app.report.top_line==0,"scroll clamps at the beginning");
 app.key(down);
 check(app.report.top_line==1&&app.argument=="cpu"&&!has_text(app,"updated source"),"scroll browses the captured report");
 check(command().x==first.x&&command().y==first.y&&command().w==first.w,"report command stays put during scrolling");
 check(std::any_of(app.nodes.begin(),app.nodes.end(),[&](const Node& n){
   return n.kind==Kind::text&&n.value==long_mixed&&n.y+n.h==viewport.top;
 })&&has_text(app,"fixture "+std::to_string(report_view_lines))&&thumb()>start,
       "the scrollbar advances and the first result stays readable in the top context band");
 for(int i=0;i<report_view_lines*3;++i)app.key(down);
 check(app.report.top_line==app.report.max_top()&&has_text(app,"MemTotal: 1234 kB"),"can reach the last line");
 app.key(up);check(app.report.top_line==app.report.max_top()-1,"scroll returns one line");
 app.key(power);
 check(app.page=="REPORT"&&app.argument.empty()&&app.focus==2,"Back restores the category");
 app.key(power);
 check(app.report.top_line==0&&has_text(app,"updated source"),"reopen takes a fresh source snapshot");
 check(std::none_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::arc;}),
       "short reports have no scrollbar");
 fixture_reads.erase(path);
 fixture_reads.erase("/proc/meminfo");
}
void reader_context_bands() {
 const std::string task="/run/overpressure/task.log",report_path="/run/overpressure/report/cpu.txt";
 std::vector<std::string> lines;
 for(int i=0;i<80;++i){
  lines.push_back("Context row "+std::to_string(i));
  fixture_reads[report_path]+=lines.back()+"\n";
 }
 fixture_logs[task]=lines;
 for(const bool report:{true,false}){
  App app;
  app.navigate(report?"REPORT":"LOG_VIEW",report?"cpu":"task");
  app.compose();
  const auto vp=reader_viewport(report);
  const auto mem=[&](int id){
   return std::find_if(app.nodes.begin(),app.nodes.end(),[&](const Node& n){return n.reader_row==id&&n.prewrapped;});
  };
  const auto check_rows=[&](int top,int scrolled=0){
   const auto first=mem(top),last=mem(top+vp.lines-1),below=mem(top+vp.lines);
   const auto fade=std::find_if(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::reader_context;});
   check(first!=app.nodes.end()&&last!=app.nodes.end()&&fade!=app.nodes.end()&&
         first->y==vp.top-scrolled&&last->y+last->h==vp.bottom()-scrolled&&
         fade->y==vp.top&&fade->y+fade->h==vp.bottom(),"the fade stays outside the fixed reader edges");
   check(first->w==app.log_width.text()&&last->w==first->w&&first->clip_y==vp.top&&
         first->clip_y+first->clip_h==vp.bottom(),"the opaque pass keeps the original body bounds and width");
   if(top>0){
    const auto above=mem(top-1);
    check(above!=app.nodes.end()&&above->reader_context&&above->y==vp.top-log_line_px-scrolled,
          "previous row appears above the core");
   }
   if(top+vp.lines<static_cast<int>(lines.size())){
    check(below!=app.nodes.end()&&below->reader_context&&below->y==vp.bottom()-scrolled,
          "next row appears below the core");
   }else check(below==app.nodes.end(),"no context is synthesized after the last result");
  };
  check_rows(0);
  if(report)app.key(down);
  else{
   app.key(down);
   for(int i=1;i<vp.lines;++i)app.key(down);
   check(app.log_top==0&&app.log_cursor==vp.lines-1,"context does not change when the viewport starts moving");
   app.key(down);
   check(app.log_top==1&&app.log_cursor==vp.lines,"the original last row remains the trigger");
  }
  check_rows(1);
  app.reader_scroll.offset_px=-17;
  app.compose();check_rows(1,17);
  app.reader_scroll.offset_px=0;
  if(report)app.report.top_line=30;
  else {app.log_top=30;app.log_cursor=30;}
  app.compose();check_rows(30);
  const auto outer_above=mem(20),outer_below=mem(30+vp.lines+8);
  check(outer_above!=app.nodes.end()&&outer_above->y<vp.top-400&&
        outer_below!=app.nodes.end()&&outer_below->y>vp.bottom()+300,
        "context extends across the eye instead of reserving only neighboring rows");
  if(report)app.report.top_line=app.report.max_top();
  else {
   app.log_top=app.log_layout.max_top();
   app.log_cursor=app.log_layout.row_count-1;
  }
  app.compose();check_rows(static_cast<int>(lines.size())-vp.lines);
 }
 fixture_logs.erase(task);
 fixture_reads.erase(report_path);
}
void log_selection_in_context() {
 std::vector<std::string> lines(130,"Selected diagnostic row");
 fixture_logs["/run/overpressure/task.log"]=lines;
 constexpr auto vp=reader_viewport(false);
 App app;
 app.navigate("LOG_VIEW","task");
 app.compose();
 app.log_back=false;
 for(const int direction:{1,-1}){
  app.log_top=30;
  app.log_cursor=app.log_top+(direction>0?vp.lines-1:0);
  const int key=direction>0?down:up;
  const double now=10;
  app.begin_reader_scroll(key,now);
  bool found=false;
  for(int ms=0;ms<=1450;ms+=5){
   const double at=now+ms/1000.;
   app.tick_reader_scroll(at);
   app.compose();
   const auto find=[&](Kind kind,bool prewrapped=false){
    return std::find_if(app.nodes.begin(),app.nodes.end(),[&](const Node& n){
      return n.reader_row==app.log_cursor&&n.kind==kind&&n.prewrapped==prewrapped;
    });
   };
   const auto row=find(Kind::box),number=find(Kind::text),marker=find(Kind::icon),body=find(Kind::text,true);
   if(row==app.nodes.end()||(row->y>=vp.top&&row->y+row->h<=vp.bottom()))continue;
   check(row->reader_context&&row->color==selection&&row->clip_y==vp.top&&
         row->clip_y+row->clip_h==vp.bottom(),"a selected row straddling the viewport keeps its colored background");
   check(number!=app.nodes.end()&&number->reader_context&&number->value==std::to_string(app.log_cursor+1)&&
         marker!=app.nodes.end()&&marker->reader_context&&marker->color==selected_ink&&
         body!=app.nodes.end()&&body->reader_context&&body->color==selected_ink,
         "selected context preserves the line number, cursor icon and foreground");
   found=true;
   break;
  }
  check(found,"a held step crosses the original edge in both directions");
  app.cancel_reader_scroll();
 }
 fixture_logs.erase("/run/overpressure/task.log");
}
void reader_motion() {
 const std::string path="/run/overpressure/report/cpu.txt";
 for(int i=0;i<160;++i)fixture_reads[path]+="fixture "+std::to_string(i)+"\n";
 fixture_logs["/run/overpressure/task.log"]=std::vector<std::string>(160,"fixture log row");
 for(const bool report:{true,false}){
  App app;
  app.navigate(report?"REPORT":"LOG_VIEW",report?"cpu":"task");
  app.compose();
  const auto row=[&]{return report?app.report.top_line:app.log_cursor;};
  if(!report)for(int i=0;i<40;++i)app.key(down);
  const int start=row();
  check(app.begin_reader_scroll(down,10)&&row()==start+1,"short press is one row immediately");
  check(!app.tick_reader_scroll(10.20)&&row()==start+1,"no repeat before hold threshold");
  app.end_reader_scroll(down,10.21);
  check(!app.tick_reader_scroll(10.70)&&row()==start+1,"releasing a tap cannot repeat");
  app.begin_reader_scroll(down,11);
  std::vector<double> steps;
  for(int ms=10;ms<=1600;ms+=10){
   const auto previous=row();
   app.tick_reader_scroll(11+ms/1000.);
   if(previous!=row()){
    if(row()!=previous+1)throw std::runtime_error("hold must emit individual rows");
    steps.push_back(ms/1000.);
   }
  }
  check(steps.size()>=7&&steps[0]>=.34&&steps[1]-steps[0]>.21&&
        steps.back()-steps[steps.size()-2]<.04,"repeat accelerates from slow to fast");
  const double deadline=app.reader_scroll.next_step;
  const int before_max_step=row();
  app.tick_reader_scroll(deadline-.001);
  check(row()==before_max_step,"held scroll cannot step before its deadline");
  app.tick_reader_scroll(deadline);
  check(row()==before_max_step+1&&std::abs(app.reader_scroll.next_step-deadline-.02)<1e-6,
        "steady repeat schedules one row every 20 ms");
  const double release=deadline+.005;
  // At 50 rows/s an expensive draw can leave a release queued beyond the
  // next 20 ms step. The prior held velocity still starts the coast.
  const auto held_motion=app.reader_scroll;
  app.end_reader_scroll(down,deadline+.090);
  check(app.reader_scroll.coasting(deadline+.090),
        "queued release during a slow frame retains the last held speed");
  app.reader_scroll=held_motion;
  app.tick_reader_scroll(release);
  const int selected=row(),offset=app.reader_scroll.offset_px;
  check(offset>0&&offset<=log_line_px,"held movement never spans more than one row");
  const auto first_px=app.reader_visual_top_px();
  app.end_reader_scroll(down,release);
  app.tick_reader_scroll(release+.09);app.compose();
  const auto early=app.reader_visual_top_px();
  app.tick_reader_scroll(release+.18);app.compose();
  const auto midway=app.reader_visual_top_px();
  check(early-first_px>2*log_line_px&&midway>early&&midway-early<early-first_px&&row()==selected,
        "release coasts forward across rows, slowing nonlinearly while selection stays locked");
  const int clip=reader_viewport(report).top;
  check(std::any_of(app.nodes.begin(),app.nodes.end(),[&](const Node& n){return n.prewrapped&&n.clip_y==clip&&n.clip_h>0;}),
        "animated content is confined to the body");
  if(report)check(std::any_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){
    return n.prewrapped&&!n.reader_context&&n.clip_h==0&&n.y==log_viewport_top+10&&n.value=="$ cat cpu.txt /proc/meminfo";
  }),"pinned command is outside the animation");
  const double resume_at=release+.18;
  const double residual=-app.reader_scroll.velocity_at(resume_at);
  app.begin_reader_scroll(down,resume_at);
  check(row()==selected+1&&std::abs(app.reader_visual_top_px()-midway)<=1,
        "same-direction re-press advances the selection without a viewport jump");
  if(report)check(!app.reader_scroll.catch_rows,"reports resume scrolling without starting a selection chase");
  check(residual>0&&std::abs(app.reader_scroll.velocity_at(resume_at)+residual)<1e-6&&
        app.reader_scroll.next_step-resume_at<.1,"same-direction re-press inherits velocity and skips first delay");
  const double resumed_step=app.reader_scroll.next_step;
  app.tick_reader_scroll(resumed_step);
  check(row()>=selected+2&&app.reader_visual_top_px()>midway&&
        std::abs(app.reader_scroll.next_step-resumed_step-.02)<1e-6,"resumed scroll rejoins the original capped ramp");
  const int released_again=row();
  app.end_reader_scroll(down,resumed_step+.005);
  app.tick_reader_scroll(resumed_step+.5);app.compose();
  const auto stopped=app.reader_visual_top_px();
  check(stopped>midway+log_line_px&&row()==released_again&&!app.reader_scroll.animating(resumed_step+.5),
        "coast rests at the new viewport without changing the release row");
  app.tick_reader_scroll(resumed_step+.7);
  check(app.reader_visual_top_px()==stopped&&row()==released_again,"resting viewport does not snap to the logical anchor");
  if(!report) {
   const int y=log_viewport_top+row()*log_line_px-static_cast<int>(stopped);
   check(y>=log_viewport_top&&y+log_line_px<=log_viewport_top+log_view_lines*log_line_px,
         "the selected log row remains visible after coasting");
  }
  // With no residual velocity, a new report tap starts at the visible row.
  // Logs still step from the locked selection, even after the viewport glided.
  const int before_reverse=report?static_cast<int>(std::lround(stopped/double(log_line_px))):row();
  app.backend.preferences.reverse=true;app.reduced_motion=true;
  app.begin_reader_scroll(down,20);
  check(row()==before_reverse-1,"reader honors reversed volume keys");
  app.tick_reader_scroll(25);
  check(row()==before_reverse-2&&app.reader_scroll.offset_px==0,"missed deadlines do not burst, reduced motion disables easing");
  app.cancel_reader_scroll();app.tick_reader_scroll(26);
  check(row()==before_reverse-2,"cancel stops repeats");
  app.begin_reader_scroll(up,27);app.back();app.tick_reader_scroll(30);
  check(!app.reader_scroll.key&&!app.reader_scroll.offset_px,"route change cancels reader motion");
 }
 fixture_reads.erase(path);
 fixture_logs.clear();
}
void log_resume_catches_edge() {
 fixture_logs["/run/overpressure/task.log"]=std::vector<std::string>(340,"fixture log row");
 for(const int direction:{1,-1}){
  App app;
  app.navigate("LOG_VIEW","task");
  app.compose();
  const int key=direction>0?down:up;
  app.log_back=false;
  app.log_top=120;
  app.log_cursor=app.log_top+(direction>0?log_view_lines-1:0);
  app.begin_reader_scroll(key,10);
  for(int ms=5;ms<=1650;ms+=5)app.tick_reader_scroll(10+ms/1000.);
  const double release=app.reader_scroll.next_step+.002;
  app.tick_reader_scroll(release);
  const double elapsed=release-app.reader_scroll.repeat_at;
  const int locked=app.log_cursor;
  app.end_reader_scroll(key,release);
  const double resume=release+.14;
  app.tick_reader_scroll(resume);
  const auto start_px=app.reader_visual_top_px();
  check(app.log_cursor==locked,"coast never chases while button is up");
  app.begin_reader_scroll(key,resume);
  check(app.reader_scroll.catch_rows>=2&&std::abs(app.reader_visual_top_px()-start_px)<=1&&
        std::abs(resume-app.reader_scroll.repeat_at-elapsed)<1e-6,
        "re-press starts a separate selection chase, preserving viewport and ramp timer");
  const auto ramp_at=app.reader_scroll.repeat_at;
  auto last_px=start_px;
  int last_row=app.log_cursor;
  bool quickened=false,reached_edge=false;
  for(int ms=5;ms<=155;ms+=5) {
   const double at=resume+ms/1000.;
   const bool catching=app.reader_scroll.catch_rows>0;
   const bool normal_due=at>=app.reader_scroll.next_step;
   app.tick_reader_scroll(at);
   const auto px=app.reader_visual_top_px();
   const int delta=(app.log_cursor-last_row)*direction;
   check(delta>=0&&(px-last_px)*direction>=0&&std::abs(px-last_px)<log_line_px,
         "catch-up does not jerk the viewport or reverse either direction");
   quickened|=catching&&delta>int(normal_due);
   if(catching&&!app.reader_scroll.catch_rows){
    const int edge=direction>0?static_cast<int>(px/log_line_px)+log_view_lines-1:
                                 static_cast<int>((px+log_line_px-1)/log_line_px);
    reached_edge=std::abs(app.log_cursor-edge)<=1;
   }
   last_px=px;last_row=app.log_cursor;
  }
  check(quickened&&reached_edge&&!app.reader_scroll.catch_rows&&app.reader_scroll.repeat_at==ramp_at,
        "log selection accelerates to the leading edge without restarting body scroll");
  app.end_reader_scroll(key,resume+.155);
  const int relocked=app.log_cursor;
  app.tick_reader_scroll(resume+.6);
  check(app.log_cursor==relocked,"release after the catch-up locks the new row");
 }
 fixture_logs.clear();
}
void populated_geometry() {
 App app;
 for (int i = 0; i < 18; ++i)
  app.backend.partitions.push_back({"fixture" + std::to_string(i), "/dev/fixture" + std::to_string(i),
                                   i == 0 ? "ext4" : "unknown", i == 1 ? "/mnt/fixture1" : "",
                                   std::uint64_t(512) << i, true});
 app.backend.partitions.push_back({"misc", "/dev/fixture-misc", "unknown", "", 4096, true});
 for (const auto* page : {"STORAGE", "HARDWARE", "REPORT"}) {
  app.page = page; app.argument.clear();
  for (const int focus : {0, 1, 7, 8, 14, 15}) {
   app.focus = focus; app.compose(); geometry(app);
  }
 }
 std::string report;
 for (int i = 0; i < 20; ++i) report += "fixture raw diagnostic line\n";
 fixture_reads["/run/overpressure/report/system.txt"] = report;
 for (const auto* argument : {"system", "cpu", "display", "unknown:999"}) {
  app.page = "REPORT"; app.argument = argument; app.focus = 0; app.compose(); geometry(app);
 }
 fixture_reads["/run/overpressure/services"] = report;
 app.page = "SERVICES"; app.compose(); geometry(app);
 for (const auto* argument : {"fixture0", "fixture1", "fixture2", "missing"}) {
  app.page = "PARTITION"; app.argument = argument; app.compose(); geometry(app);
 }
 for (const auto* key : {"probe", "hash:fixture0", "mount:fixture0", "umount:fixture1",
                        "normal", "recovery", "power:normal", "power:bootloader",
                        "power:recovery", "power:poweroff", "install", "build", "format",
                        "keys", "stock", "stockhash", "restore", "wipeuser", "wipemeta"}) {
  for (const auto* page : {"INPUTS", "TASK_DETAIL", "REVIEW", "CONFIRM", "UNAVAILABLE"}) {
   app.page = page; app.argument = key; app.focus = 0; app.compose(); geometry(app);
  }
 }
 for (const auto* state : {"running", "waiting", "done", "error"}) {
  app.backend.task.id = "fixture"; app.backend.task.name = "实际任务";
  app.backend.task.status = state; app.backend.task.locked = std::string(state) == "running";
  app.backend.task.readonly_restored = std::string(state) == "done";
  app.backend.task.detail = "实际阶段回执"; app.backend.task.error = std::string(state) == "error" ? "fixture raw error" : "";
  app.backend.task.stages = {{"检查", "done", {}}, {"写入镜像", state, {}}};
  for (const auto* page : {"CURRENT_TASK", "RUNNING", "SUCCESS", "ERROR", "HASH", "MOUNT", "EXIT", "POWER"}) {
   app.page = page; app.argument = "mount:fixture0"; app.focus = 1; app.compose(); geometry(app);
  }
 }
 app.backend.task = {};
 for (const auto d : {Depth{}, Depth{false, 16, 12, 8, 8}, Depth{true, 16, 12, 8, 8}}) {
  app.depth = d; app.draft = d;
  for (int editing = -1; editing < 4; ++editing) {
   app.page = editing == -1 ? "DISPLAY" : "LAYER_EDIT"; app.editing = editing;
   app.focus = editing + 2; app.compose(); geometry(app);
  }
  app.editing = -1;
  app.page = "TASK_DETAIL"; app.argument = "probe"; app.compose();
  app.confirm("确认只读采集", "更新已有报告，不启用硬件功能。", "probe");
  check(app.dialog && app.dialog_focus == 1, "dialog defaults to cancel at every disparity");
  app.compose(); geometry(app); app.key(power);
 }
 fixture_reads.clear();
}
void global_status() {
 App app;
 for (const auto* page : {"HOME", "LOG_VIEW"}) {
  app.page = page;
  for (const double watts : {2.5, -3.5, 0., std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
   for (const int capacity : {-1, 0, 15, 16, 100, 101}) {
    app.backend.telemetry.time_valid = true;
    app.backend.telemetry.clock = "12:34:56"; app.backend.telemetry.date = "2026-11-25 星期三";
    app.backend.telemetry.capacity = capacity; app.backend.telemetry.net_watts = watts;
    app.compose(); geometry(app);
    check(std::any_of(app.nodes.begin(),app.nodes.end(),[](const Node& n) {
     return n.kind==Kind::text && n.y==440 && n.center &&
            n.value=="12:34:56  2026-11-25";
    }), "status center contains time and date without the weekday");
    check(!has_text(app, "功率") && !has_text(app, "净充电") && !has_text(app, "净放电"),
          "status power contains only the watt value");
    if (std::isfinite(watts) && watts > 0) check(has_text(app, "+2.5 W"), "charging watts show plus sign");
    if (std::isfinite(watts) && watts < 0) check(has_text(app, "-3.5 W"), "discharging watts show minus sign");
    if (!std::isfinite(watts)) check(has_text(app, "n/a W") && !has_text(app, "nan") &&
        !has_text(app, "inf"), "unavailable watts display n/a W");
    app.backend.telemetry.net_watts.reset();
    app.compose();
    check(std::any_of(app.nodes.begin(),app.nodes.end(),[](const Node& n) {
     return n.kind==Kind::text && n.x==1392 && n.value=="n/a W" && n.ink_height==24;
    }), "missing kernel power data uses the precise watt placeholder");
    const std::string icon = "battery:" + (capacity < 0 || capacity > 100 ? std::string("unknown") : std::to_string(capacity)) + ":Unknown";
    check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
     return n.kind == Kind::icon && n.value == icon &&
            n.color.r == (capacity < 0 || capacity > 100 ? muted : capacity <= 15 ? negative : ink).r;
    }), "battery capacity and low-charge color are consistent in every status layout");
   }
  }
 }
}
void network_and_compact_progress() {
 App app;
 for (const auto* state : {"disabled", "down", "disconnected", "connecting", "no-address",
                           "connected", "suspended", "unknown"}) {
  app.backend.telemetry.ncm_state = state;
  app.backend.telemetry.upload_bps = 1023 * 1024.;
  app.backend.telemetry.download_bps = 99.9 * 1024 * 1024;
  app.page = "HOME"; app.compose(); geometry(app);
  check(has_text(app, "1.0") && has_text(app, "100") && has_text(app, "MiB/s"),
        "both traffic directions use compact three-digit byte rates");
  check(std::count_if(app.nodes.begin(),app.nodes.end(),[](const Node& n) {
   return n.kind==Kind::text && n.value=="MiB/s" && n.ink_height==20;
  })==2, "rate units remain readable alongside the normal-size numbers");
  check(std::count_if(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
   return n.kind == Kind::icon && n.value == "ncm:" + std::string(state);
  }) == 1, "each NCM state uses its own icon");
 }
 for (const auto rate : {std::optional<double>{}, std::optional<double>{-1},
                        std::optional<double>{std::numeric_limits<double>::infinity()}}) {
  app.backend.telemetry.upload_bps = rate; app.backend.telemetry.download_bps = rate;
  app.compose();
  check(has_text(app, "--") && has_text(app, "B/s"), "unobserved traffic is unknown");
 }
 for (const auto* state : {"pending", "waiting", "running", "done", "skipped", "error"}) {
  for (const auto fraction : {std::optional<double>{}, std::optional<double>{.375}}) {
   app.backend.task = {};
   app.backend.task.id = "fixture";
   app.backend.task.status = std::string_view(state) == "skipped" ? "running" : state;
   app.backend.task.stages = {{"输入检查", "done", {}}, {"系统镜像", state, fraction},
                              {"启动", "pending", {}}};
   auto tracks = [&](const char* page, double y, double height) {
    app.page = page; app.compose(); geometry(app);
    std::vector<Node> result;
    for (const auto& n : app.nodes)
     if ((n.kind == Kind::box || n.kind == Kind::stripes) &&
         n.layer == Layer::button && n.y == y && n.h == height) result.push_back(n);
    return result;
   };
   const auto regular = tracks("CURRENT_TASK", 1388, 12);
   app.log_source = "task";
   const auto compact = tracks("LOG_VIEW", 1642, 6);
   check(regular.size() == compact.size(), "compact progress retains all task stages and fills");
   for (std::size_t i = 0; i < regular.size(); ++i)
    check(regular[i].kind == compact[i].kind && regular[i].x == compact[i].x &&
          regular[i].w == compact[i].w && regular[i].color.r == compact[i].color.r &&
          regular[i].color.g == compact[i].color.g && regular[i].color.b == compact[i].color.b,
          "compact progress retains determinate, indeterminate, error and completion behavior");
  }
 }
}
void action_alignment() {
 App app;
 for (const auto* state : {"idle", "running", "done", "error"}) {
  app.backend.task = {};
  if (std::string(state) != "idle") {
   app.backend.task.id = "fixture";
   app.backend.task.name = "布局样例";
   app.backend.task.status = state;
   app.backend.task.locked = std::string(state) == "running";
   app.backend.task.readonly_restored = std::string(state) == "done";
  }
  for (const auto* page : {"CURRENT_TASK", "RUNNING", "SUCCESS", "ERROR", "HASH", "MOUNT",
                           "INPUTS", "TASK_DETAIL", "REVIEW", "UNAVAILABLE", "BOOT_ERROR"}) {
   app.page = page; app.argument = "probe"; app.focus = 1; app.compose();
   std::vector<const Node*> buttons;
   for (const auto& n : app.nodes)
    if (n.kind == Kind::box && n.layer == Layer::button && n.h == 104) buttons.push_back(&n);
   check(!buttons.empty() && buttons.size() <= 2, "task page has one or two bottom actions");
   double left = buttons[0]->x, right = left + buttons[0]->w;
   for (const auto* button : buttons) {
    left = std::min(left, button->x); right = std::max(right, button->x + button->w);
    check(std::any_of(app.nodes.begin(), app.nodes.end(), [&](const Node& n) {
     return n.kind == Kind::text && n.center && n.middle &&
            n.x + n.w / 2 == button->x + button->w / 2 && n.y == button->y && n.h == 104;
    }), "button text is centered on both axes within its container");
   }
   check((left + right) / 2 == 1080, "bottom action group is exactly centered on the page");
   if (buttons.size() == 2)
    check(buttons[1]->x - buttons[0]->x - buttons[0]->w == 16, "two-action group keeps its designed gap");
   for (const auto& n : app.nodes)
    if (n.kind == Kind::text && n.h == 62 && (n.value == "返回上一级" || n.value == "返回首页"))
     check(n.x == 396 && n.y == 684 && n.w == 276 && n.center && n.middle,
           "top-left back container is unchanged and its text is centered");
   geometry(app);
  }
 }
}
}
namespace p4ui {
Preferences load_preferences(){return loaded_preferences;}
bool Backend::has_system_image()const{return fixture_has_system;}
Preferences Backend::load_ui_preferences()const{return load_preferences();}
bool Backend::save_ui_preferences(const Preferences& p,bool,Preferences& result)const{
 return save_preferences(p.locale,result);
}
bool save_preferences(const std::string& locale, Preferences& preferences) {
 std::shared_ptr<PendingSave> attempt;
 { std::lock_guard lock(save_mutex); attempt = pending_save; }
 check(bool(attempt), "unexpected preference write");
 preference_calls++;
 attempt->locale = locale;
 attempt->thread = std::this_thread::get_id();
 attempt->started.set_value();
 const auto result = attempt->reply.get();
 if (result.throws) throw std::runtime_error("fixture preference failure");
 preferences = result.preferences;
 return result.saved;
}
bool save_settings(const Preferences& requested,Preferences& preferences) {
 return save_preferences(requested.locale,preferences);
}
std::vector<std::string> supported_locales(){return {"zh-CN","en"};}
std::string Backend::read(const std::string& path,std::size_t limit)const {
 const auto found = fixture_reads.find(path);
 return found == fixture_reads.end() ? "" : found->second.substr(0, limit);
}
std::vector<std::string> Backend::logs(const std::string& source)const {
 if (const auto found = fixture_logs.find(source); found != fixture_logs.end()) return found->second;
 std::vector<std::string> lines;for(int i=0;i<100;i++)lines.push_back("fixture entry "+std::to_string(i));return lines;
}
void Backend::refresh(bool){}
bool Backend::start(const std::string&,const std::string&,std::string&){backend_calls++;return true;}
}
int main() {
 try {
  host_install_arrival();
  first_language_saves();
  live_log_notification();
  settings_language_saves();
  localization();
  english_surface_uses_message_keys();
  log_navigation();
  log_reflow();
  report_reader();
  reader_context_bands();
  log_selection_in_context();
  reader_motion();
  log_resume_catches_edge();
  populated_geometry();
  global_status();
  network_and_compact_progress();
  action_alignment();
  App app;check(app.page=="HOME","saved language must bypass first-entry selection");app.compose();
  check(app.actions.size()>=7,"home entry points");
  app.key(116);check(app.page=="STATUS","power activates focused home row");
  app.key(115);check(app.focus==0,"one volume-up selects back");
  app.key(116);check(app.page=="HOME"&&app.focus==0,"back restores original focus");
  app.navigate("CURRENT_TASK","",1);app.compose();app.key(116);
  check(app.page=="LOG_VIEW"&&app.log_back,"overview opens full logs with back selected");
  app.key(116);check(app.page=="CURRENT_TASK"&&app.focus==1,"short power returns from default back");
  app.key(116);app.key(114);check(!app.log_back,"volume enters log selection");
  const auto before=app.log_cursor;app.key(114);check(app.log_cursor>=before,"volume moves log cursor");
  app.key(116);check(app.page=="LOG_VIEW"&&app.log_back,"power in log body focuses back");
  app.key(116);check(app.page=="CURRENT_TASK","second power returns from focused back");
  app.navigate("DISPLAY");app.compose();const auto original=app.depth;
  app.editing=0;app.page="LAYER_EDIT";for(int i=0;i<30;i++)app.key(115);
  check(app.draft.bg==16&&app.depth.bg==original.bg,"draft clamps without applying");
  app.key(116);check(app.editing==-1&&app.page=="DISPLAY","power ends one field edit");
  app.back();check(app.depth.bg==original.bg&&app.draft.bg==original.bg,"cancel restores depth draft");
  app.confirm("只读采集","fixture","probe");check(app.dialog&&app.dialog_focus==1,"confirmation defaults cancel");
  app.key(116);check(!app.dialog&&backend_calls==0,"default cancel never dispatches backend");
  app.confirm("只读采集","fixture","probe");app.key(114);app.key(116);
  check(backend_calls==1&&app.page=="RUNNING","separate explicit confirmation dispatches once");
  app.backend.task={};app.page="CURRENT_TASK";app.focus=1;app.compose();
  check(std::none_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::stripes;}),"idle task has no fabricated progress");
  app.backend.task.id="fixture";app.backend.task.status="running";app.backend.task.locked=true;
  app.backend.task.stages={{"写入镜像","running",{}}};app.compose();
  check(std::any_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::stripes;}),"unknown totals are indeterminate");
  app.backend.task.stages[0].fraction=.4;app.compose();
  check(std::none_of(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.kind==Kind::stripes;}),"trusted fraction is determinate");
  app.backend.task={};
  int pages=0;
  for(const auto* page:{"LANGUAGE","BOOT","BOOT_ERROR","CURRENT_TASK","HOME","STATUS","HARDWARE",
   "SERVICES","DEADLINE","HOST","MAINT","BUILD_MENU","STOCK_MENU","RESTORE_MENU","INPUTS",
   "TASK_DETAIL","REVIEW","CONFIRM","RUNNING","SUCCESS","ERROR","UNAVAILABLE","STORAGE","PARTITION",
   "HASH","MOUNT","DIAG","REPORT","LOGS","LOG_VIEW","POWER","BOOT_MODE","DISPLAY","LAYER_EDIT","EXIT"}) {
   for (const auto d : {Depth{}, Depth{false, 16, 12, 8, 8}, Depth{true, 16, 12, 8, 8}}) {
    app.depth=d;app.draft=d;
    app.page=page;app.argument=std::string(page)=="LANGUAGE"?"settings":"probe";app.focus=0;app.dialog=false;app.compose();
    geometry(app);
    check(has_text(app,"--:--:--") && has_text(app,"时间未校准"), "unknown time remains unknown on every page");
   }
   pages++;
  }
  app.page="UNKNOWN";app.compose();geometry(app);
  check(has_text(app,"找不到页面"),"fallback page keeps global status and original window");
  check(backend_calls==1,"composition never dispatches tasks");
  std::cout<<"MODEL_SELFTEST_OK pages="<<pages<<" geometry_checks="<<geometry_checks
           <<" preference_saves="<<preference_calls<<" backend_dispatches="<<backend_calls<<"\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
