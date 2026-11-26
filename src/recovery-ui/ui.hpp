// SPDX-License-Identifier: Unlicense
#pragma once
#include "network.hpp"
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <ctime>
#include <vector>
namespace p4ui {
struct Telemetry;
void refresh_clock(Telemetry&);
void format_clock(Telemetry&, std::int64_t);
bool display_clock(int zone,std::int64_t epoch,std::tm&);
inline constexpr int eye_size=2160, canvas_width=4320;
inline constexpr int log_width_default=1970, log_width_min=960, log_width_max=2040;
inline constexpr int log_viewport_top=568, log_viewport_height=1032;
inline constexpr int log_font_px=27, log_line_px=42;
inline constexpr int log_view_lines=log_viewport_height/log_line_px;
inline constexpr int report_command_height=64;
inline constexpr int report_view_lines=(log_viewport_height-report_command_height)/log_line_px;
inline constexpr int reader_context_transition=28, reader_context_alpha=120;
struct ReaderViewport {
 int top,lines;
 constexpr int bottom() const { return top+lines*log_line_px; }
};
constexpr ReaderViewport reader_viewport(bool report) {
 return report?ReaderViewport{log_viewport_top+report_command_height,report_view_lines}:
               ReaderViewport{log_viewport_top,log_view_lines};
}
inline constexpr int log_scroll_radius=974, log_scroll_inset=10;
struct Color { double r,g,b,a=1; bool operator==(const Color&) const = default; };
inline constexpr Color window{36/255.,39/255.,44/255.}, view{21/255.,28/255.,35/255.},
 alternate{26/255.,33/255.,42/255.}, ink{241/255.,241/255.,242/255.},
 muted{189/255.,195/255.,199/255.}, selection{86/255.,159/255.,204/255.},
 selected_ink{31/255.,31/255.,38/255.}, rule{77/255.,80/255.,85/255.},
 positive{126/255.,166/255.,75/255.}, negative{218/255.,68/255.,83/255.},
 neutral{199/255.,130/255.,61/255.}, titlebar{36/255.,39/255.,47/255.};
enum class Kind { box,text,icon,stripes,arc,reader_context };
enum class Layer { background,button,text,icon };
struct Node {
 Kind kind=Kind::box; Layer layer=Layer::background;
 double x=0,y=0,w=0,h=0,size=29.333,radius=10,border_width=0;
 Color color=view,border=rule; std::string value; bool bold=false,right=false,center=false,middle=false;
 double extra_depth=0,ink_height=0,previous_icon_gap=0;
 bool cjk_bold=false;
 bool floating_shadow=false;
 bool prewrapped=false;
 int clip_y=0,clip_h=0;
 int reader_row=-1;
 bool reader_backdrop=false;
 bool reader_context=false;
 double arc_begin=0,arc_end=0;
 bool operator==(const Node&) const = default;
};
struct LogGlyph { double advance, right; };
struct LogWidth {
 int viewport=log_width_default;
 int x() const { return (1440-viewport)/2; }
 int text() const;
};
struct LogLine { std::string text; std::size_t offset; };
struct LogEntry { std::vector<LogLine> lines; int first_row; };
struct LogDocument {
 std::vector<std::string> input;
 std::vector<LogEntry> entries;
 std::string source;
 int width=0,row_count=0;
 double sampled_at=0;
 bool english=false;
 int max_top() const { return row_count>log_view_lines?row_count-log_view_lines:0; }
};
struct ReportDocument {
 std::string id, output;
 std::vector<LogLine> lines;
 int width=0,top_line=0;
 int max_top() const {
  const auto count=static_cast<int>(lines.size());
  return count>report_view_lines?count-report_view_lines:0;
 }
 bool loaded=false;
};
enum class ReaderMotion { idle, step, coast, resume };
struct ReaderScroll {
 int key=0,direction=0,offset_px=0;
 double repeat_at=0,next_step=0,held_offset=0;
 double catch_at=0;
 int catch_rows=0,catch_done=0;
 ReaderMotion motion=ReaderMotion::idle;
 double motion_at=0,motion_from=0,motion_to=0,motion_duration=0,motion_tangent=0;
 bool animating(double now) const { return motion_duration>0 && now-motion_at<motion_duration; }
 bool coasting(double now) const { return motion==ReaderMotion::coast&&animating(now); }
 double offset_at(double now) const;
 double velocity_at(double now) const;
};
struct Depth { bool zero=false; int bg=8,button=4,text=4,icon=4; };
struct Telemetry {
 std::optional<int> capacity; std::string battery_status="Unknown";
 std::optional<double> net_watts; bool time_valid=false;
 std::string ncm_state="unknown",ncm_interface;
 std::optional<double> upload_bps,download_bps,network_sample_seconds;
 std::optional<std::uint64_t> network_rx_bytes,network_tx_bytes;
 std::string clock="--:--:--",date,time_source="system",timezone,kernel,uptime,memory,usb;
 std::int64_t epoch=0;
};
struct Substep {
 std::string id,name,state="pending";
 double weight=0;
 bool measured=false;
 std::optional<double> fraction{};
 std::string unit{};
 std::int64_t done=0,total=0;
};
struct Stage {
 std::string name,state="pending";
 std::optional<double> fraction;
 std::string id{};
 std::vector<Substep> substeps{};
};
struct Task {
 std::string id,name,status="idle",detail,error;
 std::vector<Stage> stages; int active=0; bool locked=false,readonly_restored=false;
 std::string kind;
};
// Planned time weights size each share of the parent rail. Only local counts and
// terminal phase receipts contribute to its solid fill.
inline double weighted_progress(const Stage& stage) {
 if (stage.state=="done" || stage.state=="skipped") return 1;
 double total=0,credit=0;
 for (const auto& sub : stage.substeps) {
  if (!(sub.weight>=0 && sub.weight<=86400)) return 0;
  total+=sub.weight;
  if (sub.state=="done" || sub.state=="skipped") credit+=sub.weight;
  else if ((sub.state=="running" || sub.state=="waiting" || sub.state=="error") &&
           sub.measured && sub.fraction && *sub.fraction>=0 && *sub.fraction<=1)
   credit+=sub.weight * *sub.fraction;
 }
 if (total<=0) return 0;
 const auto fraction=credit/total;
 return fraction<.999?fraction:.999;
}
bool decode_task(const std::string& input, Task& task);
struct Preferences {
 int version=1; std::string locale="zh-CN",storage="none",error;
 bool language_selected=false,durable=false;
 // Fixed, versioned preference fields. Timeouts: -1=default, 0=unlimited.
 std::uint32_t visible=0x1ff;
 int hour=24,date_style=0,zone=0,units=0,connect=-1,idle=-1;
 bool date_first=false,reverse=false;
};
inline bool same_settings(const Preferences& a,const Preferences& b) {
 return a.locale==b.locale && a.visible==b.visible && a.hour==b.hour &&
  a.date_style==b.date_style && a.zone==b.zone && a.units==b.units &&
  a.connect==b.connect && a.idle==b.idle && a.date_first==b.date_first && a.reverse==b.reverse;
}
struct Partition { std::string name,device,filesystem,mount; std::uint64_t bytes=0; bool readonly=true; };
struct Backend {
 Telemetry telemetry; Task task; Preferences preferences;
 bool install_preview=false;
 bool manual_preview=false;
 int preview_step=-1;
 NetworkRateHistory network_rates;
 std::vector<Partition> partitions;
 std::string read(const std::string& path,std::size_t limit=65536) const;
 std::vector<std::string> logs(const std::string& source) const;
 void refresh(bool include_partitions=true);
 void refresh_task();
 bool has_system_image() const;
 Preferences load_ui_preferences() const;
 bool save_ui_preferences(const Preferences& wanted,bool initial,Preferences& result) const;
 // Only prevalidated actions, never arbitrary command or user-supplied path.
 bool start(const std::string& action,const std::string& argument,std::string& error);
};
Preferences load_preferences();
bool save_preferences(const std::string& locale,Preferences& result);
bool save_preferences_native(const std::string& locale,Preferences& result);
bool save_settings(const Preferences& wanted, Preferences& result);
bool save_settings_native(const Preferences& wanted, Preferences& result);
bool decode_settings(std::string_view encoded, Preferences& result);
std::string encode_settings(const Preferences& prefs);
std::vector<std::string> supported_locales();
struct App {
 Backend backend; Depth depth,draft;
 std::string page="LANGUAGE",argument,notice,log_source="/run/overpressure/boot.log";
 struct Saved { std::string page,argument; int focus; };
 std::vector<Saved> stack; std::vector<Node> nodes;
 std::vector<std::function<void()>> actions; int focus=0,log_cursor=0,log_top=0;
 LogWidth log_width;
 LogDocument log_layout;
 // The native event loop invalidates the live document when its RAM logs change.
 bool live_log_events=false, live_log_dirty=true;
 bool follow_install_log=false,first_install_entry=false;
 std::string seen_install_task;
 ReportDocument report;
 bool report_detail() const { return (page=="REPORT"||page=="HARDWARE")&&!argument.empty(); }
 bool reader_view() const { return !dialog&&choice.empty()&&(page=="LOG_VIEW"||report_detail()); }
 ReaderScroll reader_scroll;
 bool begin_reader_scroll(int code,double now);
 void end_reader_scroll(int code,double now);
 bool tick_reader_scroll(double now);
 bool cancel_reader_scroll();
 std::int64_t reader_visual_top_px() const;
 std::function<LogGlyph(std::string_view)> measure_log_glyph;
 bool log_back=true,dialog=false; int dialog_focus=0,editing=-1;
 std::string dialog_title,dialog_body,dialog_action,dialog_argument;
 std::future<Preferences> language_save; bool language_save_initial=false;
 std::optional<Preferences> pending_save,uncommitted;
 std::optional<Preferences> saving_settings;
 std::string save_error;
 double retry_save_at=0;
 std::string choice;
 std::vector<std::pair<int,std::string>> choice_options;
 int choice_focus=0;
 std::optional<Preferences> deadline_confirmation;
 std::string held_power;
 bool power_held=false;
 bool reduced_motion=false;
 std::size_t overlay_begin=0,status_begin=0;
 const Preferences& settings() const { return uncommitted?*uncommitted:backend.preferences; }
 void change_settings(Preferences next, bool initial=false, bool deadline_authorized=false);
 void render_settings();
 std::string activation() const;
 std::uint64_t frame=0; double animation_phase=0;
 explicit App(bool preview=false, bool defer_startup=false);
 void finish_startup();
 void navigate(const std::string& id,const std::string& arg="",int initial=1);
 void back(); void key(int code); void compose();
 bool observe_install_task();
 void confirm(const std::string& title,const std::string& body,const std::string& action,const std::string& arg="");
private:
 void open_home(int focus=0);
 void open_install_log();
 bool step_reader(int direction,bool repeating=false);
 int reader_top() const { return report_detail()?report.top_line:log_top; }
 int reader_max_top() const { return report_detail()?report.max_top():log_layout.max_top(); }
 double reader_bounded_top(double px,bool keep_selection=false) const;
 int log_leading_row(double visual,int direction) const;
 bool catch_log_selection(double now);
};
double monotonic_seconds();
}
