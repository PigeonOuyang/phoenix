// SPDX-License-Identifier: Unlicense
// Native recovery compositor. The screenshot socket accepts capture only.
#include "ui.hpp"
#include "breeze_render.hpp"
#include "settings_codec.hpp"
#include "optics.hpp"
#include "log_layout.hpp"
#include "progress_layout.hpp"
#include "log_progress_policy.hpp"
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <json-c/json.h>
#include <linux/input.h>
#include <png.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
namespace fs=std::filesystem;
using namespace p4ui;
namespace {
volatile sig_atomic_t stopping=0;
void stop(int) { stopping=1; }
void require(bool b,const std::string& s) { if(!b) throw std::runtime_error(s+": "+std::strerror(errno)); }
void source(cairo_t* c,Color color) { cairo_set_source_rgba(c,color.r,color.g,color.b,color.a); }
void rounded(cairo_t* c,double x,double y,double w,double h,double r) {
 r=std::min({r,w/2,h/2}); cairo_new_sub_path(c);
 cairo_arc(c,x+w-r,y+r,r,-M_PI/2,0); cairo_arc(c,x+w-r,y+h-r,r,0,M_PI/2);
 cairo_arc(c,x+r,y+h-r,r,M_PI/2,M_PI); cairo_arc(c,x+r,y+r,r,M_PI,3*M_PI/2);
 cairo_close_path(c);
}
constexpr const char* optics_path="/run/overpressure/optics-ring";
constexpr const char* log_width_path="/run/overpressure/log-width";
int boot_seconds() {
 timespec value{};
 if(clock_gettime(CLOCK_BOOTTIME,&value)!=0)return 0;
 return static_cast<int>(value.tv_sec);
}
void startup_mark(const char* stage) {
 const std::string line="Recovery UI: "+std::string(stage);
 std::cout<<line<<'\n';
 for(const auto* path:{"/dev/kmsg","/dev/pmsg0"}){
  const int fd=open(path,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
  if(fd<0)continue;
  const std::string message=line+"\n";
  (void)write(fd,message.data(),message.size());
  close(fd);
 }
}
void draw_optics(unsigned char* data,int pitch,const OpticsRing& ring) {
 auto* surface=cairo_image_surface_create_for_data(data,CAIRO_FORMAT_RGB24,eye_size,eye_size,pitch);
 auto* c=cairo_create(surface);
 cairo_set_source_rgb(c,0,0,0);cairo_paint(c);
 cairo_set_source_rgb(c,.82,.82,.82);
 cairo_set_line_width(c,ring.stroke);
 cairo_arc(c,ring.x,ring.y,(ring.diameter-ring.stroke)/2.0,0,2*M_PI);
 cairo_stroke(c);
 // Small, dim fixation point at the reference center of this ring.
 cairo_set_source_rgb(c,.32,.32,.32);
 cairo_arc(c,ring.x,ring.y,3,0,2*M_PI);cairo_fill(c);
 const auto status=cairo_status(c);
 cairo_destroy(c);cairo_surface_flush(surface);cairo_surface_destroy(surface);
 require(status==CAIRO_STATUS_SUCCESS,"绘制视野测量圆");
 for(int y=0;y<eye_size;++y)
  std::memcpy(data+y*pitch+eye_size*4,data+y*pitch,eye_size*4);
}
struct Fonts {
 FT_Library lib{}; std::array<FT_Face,4> face{}; std::array<cairo_font_face_t*,4> cairo{};
 explicit Fonts(const std::string& dir) {
  require(FT_Init_FreeType(&lib)==0,"初始化字体");
  std::array<std::string,4> names{"NotoSans-Regular.ttf","NotoSans-Bold.ttf",
   "NotoSansCJK-SC-Regular.otf","NotoSansCJK-SC-Bold.otf"};
  for(int i=0;i<4;i++) {
   require(FT_New_Face(lib,(dir+"/"+names[i]).c_str(),0,&face[i])==0,"载入字体 "+names[i]);
   cairo[i]=cairo_ft_font_face_create_for_ft_face(face[i],0);
  }
 }
 ~Fonts() { for(auto* f:cairo) if(f)cairo_font_face_destroy(f); for(auto f:face) if(f)FT_Done_Face(f); if(lib)FT_Done_FreeType(lib); }
 int select(unsigned cp,bool bold) { int i=(cp<128?0:2)+(bold?1:0); return FT_Get_Char_Index(face[i],cp)?i:(bold?3:2); }
};
struct Character { std::string bytes; unsigned cp=0; };
std::vector<Character> characters(const std::string& text) {
 std::vector<Character> result;
 for(std::size_t i=0;i<text.size();) {
  auto a=static_cast<unsigned char>(text[i]); std::size_t n=a<0x80?1:(a&0xe0)==0xc0?2:(a&0xf0)==0xe0?3:(a&0xf8)==0xf0?4:1;
  if(i+n>text.size()) n=1;
  unsigned cp=n==1?a:a&((1u<<(7-n))-1); bool valid=n>1||a<128;
  for(std::size_t j=1;j<n;j++) { auto b=static_cast<unsigned char>(text[i+j]); if((b&0xc0)!=0x80)valid=false; cp=(cp<<6)|(b&63); }
  if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||
     (cp>=0xd800&&cp<=0xdfff)||cp>0x10ffff)valid=false;
  if(!valid) { result.push_back({"\xef\xbf\xbd",0xfffd}); ++i; }
  else { result.push_back({text.substr(i,n),cp}); i+=n; }
 }
 return result;
}
class Painter {
 Fonts fonts;
 BreezeAtlas breeze;
 std::string page_key,overlay_key;
 std::vector<Node> base_previous,base_current,overlay_previous,overlay_current;
 std::map<std::string,LogGlyph,std::less<>> log_glyphs;
 using ProgressMetric=std::tuple<std::string,double,bool>;
 std::map<ProgressMetric,double> progress_metrics;
 progress_ui::Animation progress_motion;
 std::optional<log_progress::DeckInput> progress_input;
 std::vector<Node> progress_nodes;
 bool legacy_progress_moving_=false;
 using ReaderGlyphs=std::array<std::vector<cairo_glyph_t>,4>;
 std::unordered_map<std::string,ReaderGlyphs> reader_glyphs;
 std::array<cairo_scaled_font_t*,4> reader_fonts{};
 struct RasterRow {
  std::vector<Node> nodes;
  std::vector<std::uint32_t> pixels;
 };
 std::map<int,RasterRow> reader_rows;
 std::string reader_key;
 Node reader_background;
 std::vector<std::uint32_t> reader_blank;
 std::vector<std::uint32_t> reader_chrome;
 std::array<int,4> reader_depth{};
 double page_at=0,overlay_at=0,overlay_time=.22;
 bool animating_=false;
 int select(const Node& n,unsigned cp) {
  return fonts.select(cp,n.bold||(n.cjk_bold&&cp>=0x2e80));
 }
 void font(cairo_t* c,int i,double size) { cairo_set_font_face(c,fonts.cairo[i]); cairo_set_font_size(c,size); }
 void reader_line(cairo_t* c,const Node& n) {
  for(int i=0;i<4;++i)if(!reader_fonts[i]){
   font(c,i,log_font_px);reader_fonts[i]=cairo_scaled_font_reference(cairo_get_scaled_font(c));
  }
  auto it=reader_glyphs.find(n.value);
  if(it==reader_glyphs.end()){
   if(reader_glyphs.size()>=1024)reader_glyphs.clear();
   ReaderGlyphs glyphs;
   double x=0;
   for(const auto& ch:characters(n.value)){
    const auto i=select(n,ch.cp);
    const cairo_glyph_t glyph{FT_Get_Char_Index(fonts.face[i],ch.cp),x,0};
    glyphs[i].push_back(glyph);
    cairo_text_extents_t ext{};
    cairo_scaled_font_glyph_extents(reader_fonts[i],&glyph,1,&ext);
    x+=ext.x_advance;
   }
   it=reader_glyphs.emplace(n.value,std::move(glyphs)).first;
  }
  cairo_translate(c,n.x,n.y+n.size*1.16);
  for(int i=0;i<4;++i)if(!it->second[i].empty()){
   cairo_set_scaled_font(c,reader_fonts[i]);
   cairo_show_glyphs(c,it->second[i].data(),static_cast<int>(it->second[i].size()));
  }
 }
 std::array<double,4> optical_sizes(cairo_t* c,const Node& n) {
  std::array<double,4> sizes{n.size,n.size,n.size,n.size};
  if(n.ink_height<=0)return sizes;
  std::array<std::string,4> references;
  for(const auto& ch:characters(n.value))references[select(n,ch.cp)]+=ch.bytes;
  for(int i=0;i<4;++i)if(!references[i].empty()) {
   font(c,i,n.size);cairo_text_extents_t ext{};
   cairo_text_extents(c,references[i].c_str(),&ext);
   // Punctuation-only placeholders must not expand into giant dash glyphs.
   if(ext.height<n.size*.4)references[i]=i<2?"0123456789":"国";
   for(int attempt=0;attempt<5;++attempt) {
    font(c,i,sizes[i]);cairo_text_extents(c,references[i].c_str(),&ext);
    if(ext.height<=0||std::abs(ext.height-n.ink_height)<.01)break;
    sizes[i]*=n.ink_height/ext.height;
   }
  }
  return sizes;
 }
public:
 struct TextRun {std::string value;double x,y,w,h,target;Color color;int face;};
 std::vector<TextRun> status_runs;
 explicit Painter(const std::string& dir,const std::string& atlas):fonts(dir),breeze(atlas) {}
 ~Painter(){for(auto* f:reader_fonts)if(f)cairo_scaled_font_destroy(f);}
 bool animating() const { return animating_; }
 bool progress_moving() const {
  return progress_input&&!progress_input->reduced_motion&&(progress_motion.tween||progress_motion.activity);
 }
 bool legacy_progress_moving() const { return legacy_progress_moving_; }
 bool stable_reader(const App& app) const {
  return app.reader_view()&&!animating_&&overlay_key.empty()&&page_key==app.page+":"+app.argument;
 }
 std::array<double,2> icon_bounds(const Node& n) const {
  const auto b=breeze.metrics(n);
  const auto width=b[2]*std::min(n.ink_height/b[3],n.w/b[2]);
  return {n.x+(n.w-width)/2,n.x+(n.w+width)/2};
 }
 std::array<double,3> line_metrics(const Node& n) {
  auto* surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,1,1);
  auto* c=cairo_create(surface);
  cairo_font_options_t* options=cairo_font_options_create();
  cairo_font_options_set_hint_metrics(options,CAIRO_HINT_METRICS_ON);
  cairo_set_font_options(c,options);cairo_font_options_destroy(options);
  const auto sizes=optical_sizes(c,n);double width=0,left=0,right=0;bool ink=false;
  for(const auto& ch:characters(n.value)) {
   const int index=select(n,ch.cp);
   font(c,index,sizes[index]);cairo_text_extents_t ext{};
   cairo_text_extents(c,ch.bytes.c_str(),&ext);
   if(ext.width>0&&ext.height>0) {
    const double a=width+ext.x_bearing,b=a+ext.width;
    if(!ink){left=a;right=b;ink=true;}else{left=std::min(left,a);right=std::max(right,b);}
   }
   width+=ext.x_advance;
  }
  cairo_destroy(c);cairo_surface_destroy(surface);return {width,left,right};
 }
 double line_width(const Node& n) {return line_metrics(n)[0];}
 double progress_width(std::string_view value,double size,bool bold) {
  ProgressMetric key{value,size,bold};
  if(const auto it=progress_metrics.find(key);it!=progress_metrics.end())return it->second;
  Node n;n.value=value;n.size=size;n.bold=bold;
  const auto [advance,left,right]=line_metrics(n);
  const double width=std::max(advance,right)-std::min(0.,left);
  if(progress_metrics.size()>=2048)progress_metrics.clear();
  progress_metrics.emplace(std::move(key),width);
  return width;
 }
 void bind_log_metrics(App& app) {
  app.measure_log_glyph=[this](std::string_view s) {
   if(const auto it=log_glyphs.find(s);it!=log_glyphs.end()) return it->second;
   Node n;n.size=log_font_px;n.value=s;
   const auto result=line_metrics(n);
   const LogGlyph width{result[0],result[2]};
   if(log_glyphs.size()<8192)log_glyphs.emplace(s,width);
   return width;
  };
 }
 void text(cairo_t* c,const Node& n) {
  cairo_save(c); cairo_rectangle(c,n.x,n.y,n.w,n.h); cairo_clip(c);
  source(c,n.color); const double lineheight=n.size*1.5;
  if(n.prewrapped){
   reader_line(c,n);
   cairo_restore(c);cairo_new_path(c);return;
  }
  const auto sizes=optical_sizes(c,n);
  struct Run { std::string s; int font; double width; };
  struct Line {std::vector<Run> runs;double advance,left,right,top,bottom;bool ink;};
  std::vector<Line> lines;
  std::vector<Run> runs; double width=0,baseline=n.y+n.size*1.16;
  auto flush=[&] {
   if(n.center||n.middle) {
    Line line{{},width,0,0,0,0,false};double x=0;
    for(auto& r:runs) {
     font(c,r.font,sizes[r.font]);cairo_text_extents_t ext{};cairo_text_extents(c,r.s.c_str(),&ext);
     if(ext.width>0&&ext.height>0) {
      const double left=x+ext.x_bearing,right=left+ext.width;
      if(!line.ink){line.left=left;line.right=right;line.top=ext.y_bearing;line.bottom=ext.y_bearing+ext.height;line.ink=true;}
      else {line.left=std::min(line.left,left);line.right=std::max(line.right,right);
       line.top=std::min(line.top,ext.y_bearing);line.bottom=std::max(line.bottom,ext.y_bearing+ext.height);}
     }
     x+=r.width;
    }
    line.runs=std::move(runs);lines.push_back(std::move(line));
   } else {
    double x=n.right?n.x+n.w-width:n.x;
    for(auto& r:runs) { font(c,r.font,sizes[r.font]); cairo_move_to(c,x,baseline); cairo_show_text(c,r.s.c_str()); x+=r.width; }
   }
   runs.clear(); width=0; baseline+=lineheight;
  };
  for(const auto& ch:characters(n.value)) {
   if(ch.cp=='\n') { flush(); if(baseline>n.y+n.h+n.size)break; continue; }
   const int index=select(n,ch.cp); font(c,index,sizes[index]);
   cairo_text_extents_t ext{}; cairo_text_extents(c,ch.bytes.c_str(),&ext);
   if(width>0 && width+ext.x_advance>n.w) { flush(); if(baseline>n.y+n.h+n.size)break; }
   if(!runs.empty()&&runs.back().font==index) { runs.back().s+=ch.bytes; runs.back().width+=ext.x_advance; }
   else runs.push_back({ch.bytes,index,ext.x_advance});
   width+=ext.x_advance;
  }
  if(!runs.empty())flush();
  if(!lines.empty()) {
   double top=0,bottom=0;bool has_ink=false;
   for(std::size_t i=0;i<lines.size();++i)if(lines[i].ink) {
    const double a=lines[i].top+i*lineheight,b=lines[i].bottom+i*lineheight;
    if(!has_ink){top=a;bottom=b;has_ink=true;}else{top=std::min(top,a);bottom=std::max(bottom,b);}
   }
   const double first_baseline=n.middle&&has_ink?n.y+(n.h-bottom+top)/2-top:n.y+n.size*1.16;
   for(std::size_t i=0;i<lines.size();++i) {
    const auto& line=lines[i];
    double x=n.center&&line.ink?n.x+(n.w-line.right+line.left)/2-line.left:
     n.right?n.x+n.w-line.advance:n.x;
    for(auto& r:line.runs) {
     font(c,r.font,sizes[r.font]);double baseline=first_baseline+i*lineheight;
     if(n.ink_height>0) {
      cairo_text_extents_t ext{};cairo_text_extents(c,r.s.c_str(),&ext);
      baseline=n.y+(n.h-ext.height)/2-ext.y_bearing;
      status_runs.push_back({r.s,x,n.y,r.width,n.h,n.ink_height,n.color,r.font});
     }
     cairo_move_to(c,x,baseline);cairo_show_text(c,r.s.c_str());x+=r.width;
    }
   }
  }
  cairo_restore(c); cairo_new_path(c);
 }
 void arc(cairo_t* c,const Node& n) {
  cairo_new_path(c);source(c,n.color);
  cairo_set_line_width(c,n.border_width);cairo_set_line_cap(c,CAIRO_LINE_CAP_ROUND);
  cairo_arc(c,n.x+n.w/2,n.y+n.h/2,n.w/2,n.arc_begin,n.arc_end);
  cairo_stroke(c);
 }
 void paint_reader_item(cairo_t* c,const Node& n) {
  if(n.kind==Kind::text)text(c,n);
  else if(n.kind==Kind::icon)breeze.draw(c,n);
  else if(n.kind==Kind::box){
   rounded(c,n.x,n.y,n.w,n.h,n.radius);source(c,n.color);cairo_fill(c);
  }
 }
 void paint_reader_context(cairo_t* c,const std::vector<Node>& nodes,const std::array<int,4>& depth,
                           int eye,int first,int last) {
  const auto marker=std::find_if(nodes.begin(),nodes.end(),[](const Node& n){return n.kind==Kind::reader_context;});
  if(marker==nodes.end())return;
  const int top=static_cast<int>(marker->y),bottom=static_cast<int>(marker->y+marker->h);
  if(first>=top&&last<=bottom)return;
  const auto bg=std::find_if(nodes.begin(),nodes.end(),[](const Node& n){return n.reader_backdrop;});
  if(bg==nodes.end())return;
  const bool progress_deck=std::any_of(nodes.begin(),nodes.end(),[](const Node& n){
   return n.kind==Kind::box&&n.value=="progress-deck";
  });
  const double sign=eye?-1.:1.;
  const auto visible=[&](const Node& n){
   return n.reader_context&&n.y+n.h>first&&n.y<last&&(n.y<top||n.y+n.h>bottom);
  };
  cairo_save(c);
  cairo_rectangle(c,0,0,eye_size,top);
  cairo_rectangle(c,0,bottom,eye_size,eye_size-bottom);
  cairo_clip(c);
  cairo_push_group(c);
  // Composite the entire pane first so the ground, row stripes, line numbers
  // and active row fade as a single layer. The panel's optics set the visible edge.
  cairo_rectangle(c,bg->x+sign*depth[static_cast<int>(bg->layer)]/2,0,bg->w,eye_size);
  source(c,bg->color);cairo_fill(c);
  for(const auto& original:nodes){
   if(!visible(original))continue;
   auto n=original;
   n.x+=sign*depth[static_cast<int>(n.layer)]/2;
   paint_reader_item(c,n);
  }
  cairo_pop_group_to_source(c);
  auto* mask=cairo_pattern_create_linear(0,0,0,eye_size);
  const auto stop=[&](double y,double alpha){
   cairo_pattern_add_color_stop_rgba(mask,y/eye_size,0,0,0,alpha);
  };
  constexpr double fade=reader_context_alpha/255.;
  for(int i=0;i<=4;++i){
   const double t=i/4.;
   stop(top-reader_context_transition*(1-t),std::lerp(fade,1.,t*t*(3-2*t)));
  }
  for(int i=0;i<=4;++i){
   const double t=i/4.;
   stop(bottom+reader_context_transition*t,std::lerp(1.,fade,t*t*(3-2*t)));
  }
  if(progress_deck)stop(progress_ui::child_top,0);
  cairo_pattern_set_extend(mask,CAIRO_EXTEND_PAD);
  cairo_mask(c,mask);
  cairo_pattern_destroy(mask);
  cairo_restore(c);
 }
 void reader_body(unsigned char* data,int pitch,const App& app) {
  const auto& d=app.depth;
  const std::array<int,4> depth=d.zero?std::array<int,4>{0,0,0,0}:
    std::array<int,4>{-d.bg,d.button,d.button+d.text,d.button+d.text+d.icon};
  const auto bg=std::find_if(app.nodes.begin(),app.nodes.end(),[](const Node& n){return n.reader_backdrop;});
  require(bg!=app.nodes.end(),"读取正文背景");
  const auto key=app.page+":"+app.argument+":"+app.log_source;
  constexpr int row_bytes=canvas_width*4;
  if(key!=reader_key||*bg!=reader_background||depth!=reader_depth){
   reader_key=key;reader_background=*bg;reader_depth=depth;
   reader_rows.clear();reader_blank.clear();
  }
  const auto viewport=reader_viewport(app.report_detail());
  const int first=viewport.top,last=viewport.bottom();
  const auto rasterize=[&](std::vector<std::uint32_t>& pixels,const std::vector<Node>& nodes){
   pixels.resize(canvas_width*log_line_px);
   // Render on cacheable memory, then write whole opaque lines to the scanout buffer.
   for(int eye=0;eye<2;eye++){
    if(eye&&d.zero){
     for(int y=0;y<log_line_px;y++)
      std::memcpy(pixels.data()+y*canvas_width+eye_size,pixels.data()+y*canvas_width,eye_size*4);
     break;
    }
    auto* surface=cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels.data()+eye*eye_size),
      CAIRO_FORMAT_RGB24,eye_size,log_line_px,row_bytes);
    auto* c=cairo_create(surface);
    auto* options=cairo_font_options_create();
    cairo_font_options_set_antialias(options,CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_metrics(options,CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(c,options);cairo_font_options_destroy(options);
    source(c,window);cairo_paint(c);
    const double sign=eye?-1.:1.;
    source(c,bg->color);cairo_rectangle(c,bg->x+sign*depth[0]/2,0,bg->w,log_line_px);cairo_fill(c);
    for(auto node:nodes){
     node.x+=sign*depth[static_cast<int>(node.layer)]/2;
     paint_reader_item(c,node);
    }
    require(cairo_status(c)==CAIRO_STATUS_SUCCESS,"绘制正文行");
    cairo_destroy(c);cairo_surface_flush(surface);cairo_surface_destroy(surface);
   }
  };
  if(reader_blank.empty())rasterize(reader_blank,{});
  if(reader_rows.size()>64){
   const auto visible_first=app.reader_visual_top_px()/log_line_px;
   std::erase_if(reader_rows,[&](const auto& row){return row.first-visible_first< -8||row.first-visible_first>32;});
  }
  int filled=first;
  const auto blit=[&](const std::vector<std::uint32_t>& pixels,int y,int from,int count){
   auto* dest=data+y*pitch;
   const auto* src=pixels.data()+from*canvas_width;
   if(pitch==row_bytes)std::memcpy(dest,src,static_cast<std::size_t>(count)*row_bytes);
   else for(int line=0;line<count;line++)std::memcpy(dest+line*pitch,src+line*canvas_width,row_bytes);
  };
  const auto blank_to=[&](int end){
   while(filled<end){
    const int count=std::min(end-filled,log_line_px);
    blit(reader_blank,filled,0,count);filled+=count;
   }
  };
  for(auto at=app.nodes.begin();at!=app.nodes.end();){
   if(at->reader_row<0){++at;continue;}
   const auto begin=at;
   const int id=at->reader_row, y=static_cast<int>(at->y);
   while(at!=app.nodes.end()&&at->reader_row==id)++at;
   const int from=std::max(begin->clip_y,y),to=std::min(y+log_line_px,begin->clip_y+begin->clip_h);
   if(to<=from)continue;
   auto& row=reader_rows[id];
   const auto normalized=[&](Node n){n.y-=y;return n;};
   if(row.nodes.size()!=static_cast<std::size_t>(at-begin)||
      !std::equal(begin,at,row.nodes.begin(),[&](const Node& n,const Node& cached){return normalized(n)==cached;})){
    row.nodes.clear();
    std::transform(begin,at,std::back_inserter(row.nodes),normalized);
    rasterize(row.pixels,row.nodes);
   }
   blank_to(from);blit(row.pixels,from,from-y,to-from);filled=to;
  }
  blank_to(last);
  for(int eye=0;eye<2;eye++){
   auto* s=cairo_image_surface_create_for_data(data+first*pitch+eye*eye_size*4,CAIRO_FORMAT_RGB24,
     eye_size,last-first,pitch);
   auto* c=cairo_create(s);
   cairo_translate(c,0,-first);
   for(const auto& n:app.nodes)if(n.kind==Kind::arc)arc(c,n);
   require(cairo_status(c)==CAIRO_STATUS_SUCCESS,"绘制弧形滚动条");
   cairo_destroy(c);cairo_surface_flush(s);cairo_surface_destroy(s);
  }
 }
 void reader_scene(unsigned char* data,int pitch,App& app) {
  // Cairo's alpha blends read the destination. Draw reader chrome in ordinary
  // cached memory and only stream finished rows into the mapped KMS buffer.
  constexpr int row_bytes=canvas_width*4;
  if(reader_chrome.empty())reader_chrome.resize(static_cast<std::size_t>(canvas_width)*eye_size);
  auto* pixels=reinterpret_cast<unsigned char*>(reader_chrome.data());
  scene(pixels,row_bytes,app,true);
  const auto viewport=reader_viewport(app.report_detail());
  const auto blit=[&](int first,int last){
   if(pitch==row_bytes)std::memcpy(data+first*pitch,pixels+first*row_bytes,static_cast<std::size_t>(last-first)*row_bytes);
   else for(int y=first;y<last;++y)std::memcpy(data+y*pitch,pixels+y*row_bytes,row_bytes);
  };
  blit(0,viewport.top);
  blit(viewport.bottom(),eye_size);
  reader_body(data,pitch,app);
 }
 void scene(unsigned char* data,int pitch,App& app,bool chrome_only=false) {
  status_runs.clear();
  // Pack the battery against the actual percentage ink at all digit counts.
  for(std::size_t i=1;i<app.nodes.size();++i) {
   const auto& text=app.nodes[i];auto& icon=app.nodes[i-1];
   if(text.previous_icon_gap<=0||text.kind!=Kind::text||icon.kind!=Kind::icon)continue;
   const auto metrics=line_metrics(text);
   const auto bounds=breeze.metrics(icon);
   const double scale=std::min(icon.ink_height/bounds[3],icon.w/bounds[2]);
   const double ink_left=text.x+text.w-metrics[0]+metrics[1];
   icon.x=ink_left-text.previous_icon_gap-(icon.w+bounds[2]*scale)/2;
  }
  const auto d=app.page=="DISPLAY"||app.page=="LAYER_EDIT"?app.draft:app.depth;
  const std::array<int,4> depth=d.zero?std::array<int,4>{0,0,0,0}:std::array<int,4>{-d.bg,d.button,d.button+d.text,d.button+d.text+d.icon};
  const auto key=app.page+":"+app.argument,next_overlay=app.dialog?"dialog:"+app.dialog_action+app.dialog_argument:app.choice;
  const double now=app.animation_phase;
  const bool motion=app.frame>0&&now>0&&!app.reduced_motion;
  if(key!=page_key){
   base_previous=motion?std::move(base_current):std::vector<Node>{};
   page_at=now;page_key=key;
  }
  if(next_overlay!=overlay_key){
   overlay_previous=motion&&next_overlay.empty()?std::move(overlay_current):std::vector<Node>{};
   overlay_time=(next_overlay.empty()?overlay_key:next_overlay).starts_with("dialog:")?.25:.22;
   overlay_at=now;overlay_key=next_overlay;
  }
  base_current.assign(app.nodes.begin(),app.nodes.begin()+app.overlay_begin);
  legacy_progress_moving_=!app.reduced_motion&&std::ranges::any_of(base_current,[](const Node& n) {
   return n.kind==Kind::stripes&&n.value.empty();
  });
  if(progress_ui::has_log_deck(app)){
   const bool changed=!progress_input||!progress_input->matches(app);
   if(changed)progress_input.emplace(app);
   if(changed||progress_motion.tween){
    progress_nodes.clear();
    progress_ui::draw_log_deck(app,progress_motion,[this](auto value,double size,bool bold) {
     return progress_width(value,size,bold);
    },progress_nodes);
   }
   base_current.insert(base_current.end(),progress_nodes.begin(),progress_nodes.end());
  }else if(progress_input){
   progress_input.reset();
   progress_motion={};
   progress_nodes.clear();
  }
  overlay_current.assign(app.nodes.begin()+app.overlay_begin,app.nodes.begin()+app.status_begin);
  // InOutQuad, as used by the KDE desktop-style menu transitions.
  const auto ease=[](double v){v=std::clamp(v,0.,1.);return v<.5?2*v*v:1-2*(1-v)*(1-v);};
  const double page_alpha=motion&&!base_previous.empty()?ease((now-page_at)/.25):1.;
  const double layer_alpha=motion?ease((now-overlay_at)/overlay_time):1.;
  if(page_alpha>=1)base_previous.clear();
  if(layer_alpha>=1)overlay_previous.clear();
  animating_=page_alpha<1||((!overlay_current.empty()||!overlay_previous.empty())&&layer_alpha<1);
  const auto draw_band=[&](int first_y,int last_y){
  for(int eye=0;eye<2;eye++) {
   if(eye&&d.zero) {
    for(int y=first_y;y<last_y;y++) std::memcpy(data+y*pitch+eye_size*4,data+y*pitch,eye_size*4);
    break;
   }
   auto* s=cairo_image_surface_create_for_data(data+first_y*pitch+eye*eye_size*4,CAIRO_FORMAT_RGB24,
      eye_size,last_y-first_y,pitch);
   auto* c=cairo_create(s);
   cairo_translate(c,0,-first_y);
   source(c,window);cairo_paint(c);
   cairo_font_options_t* options=cairo_font_options_create();cairo_font_options_set_antialias(options,CAIRO_ANTIALIAS_GRAY);
   cairo_font_options_set_hint_metrics(options,CAIRO_HINT_METRICS_ON);cairo_set_font_options(c,options);cairo_font_options_destroy(options);
   auto paint_node=[&](const Node& original) {
    if(original.kind==Kind::reader_context)return;
    if(original.kind==Kind::arc) {
     // Lens-space guide: the optical measurement used identical per-eye coordinates.
     cairo_save(c);arc(c,original);cairo_restore(c);return;
    }
    Node n=original;
    const bool scrim=n.w==eye_size&&n.h==eye_size;
    if (!scrim)n.x+=(eye==0?1:-1)*(depth[static_cast<int>(n.layer)]+(d.zero?0:n.extra_depth))/2.;
    if(n.kind==Kind::text) { text(c,n);return; }
    if(n.kind==Kind::icon) { breeze.draw(c,n);return; }
    if(n.floating_shadow) {
     for(int radius=16;radius>=2;radius-=2){
      rounded(c,n.x-radius/2.,n.y+4-radius/2.,n.w+radius,n.h+radius,n.radius+radius/2.);
      source(c,Color{0,0,0,.043});cairo_fill(c);
     }
    }
    const auto rectangle=[&]{
     if(n.radius>0)rounded(c,n.x,n.y,n.w,n.h,n.radius);
     else cairo_rectangle(c,n.x,n.y,n.w,n.h);
    };
    if(n.kind==Kind::stripes&&n.value.starts_with("progress-")) {
     if(n.color.a<=0)return;
     const bool skip=n.value.starts_with("progress-skip");
     const bool child=n.value.starts_with("progress-child")||n.value=="progress-skip-child";
     cairo_save(c);rectangle();cairo_clip(c);
     const double pitch=(child?(skip?10:19.8):(skip?16:20))*progress_ui::scale;
     if(skip) {
      source(c,Color{135/255.,145/255.,153/255.,.7*n.color.a});
      for(double x=n.x;x<n.x+n.w;x+=pitch)
       cairo_rectangle(c,x,n.y,(child?6:9)*progress_ui::scale,n.h);
     } else {
      source(c,Color{42/255.,83/255.,110/255.,n.color.a});
      cairo_paint(c);source(c,n.color);
      const bool still=app.reduced_motion||n.value.ends_with("-still");
      const double phase=still?0:std::fmod(app.animation_phase/(child?1.25:1.),1.)*pitch;
      const double stripe=5.66*progress_ui::scale;
      for(double x=n.x-pitch+phase;x<n.x+n.w+n.h;x+=pitch) {
       cairo_move_to(c,x,n.y+n.h);cairo_line_to(c,x+stripe,n.y+n.h);
       cairo_line_to(c,x+stripe+n.h,n.y);cairo_line_to(c,x+n.h,n.y);cairo_close_path(c);
      }
     }
     cairo_fill(c);cairo_restore(c);return;
    }
    rectangle();source(c,n.color);cairo_fill_preserve(c);
    if(n.border_width>0) {source(c,n.border);cairo_set_line_width(c,n.border_width);cairo_stroke(c);}else cairo_new_path(c);
    if(n.kind==Kind::stripes) {
     cairo_save(c);rounded(c,n.x,n.y,n.w,n.h,n.radius);cairo_clip(c);source(c,selection);
     const double phase=app.reduced_motion||!n.value.empty()?0:std::fmod(app.animation_phase*38.,40.);
     for(double x=n.x-20+phase;x<n.x+n.w+30;x+=20) {
      cairo_move_to(c,x,n.y+n.h);cairo_line_to(c,x+10,n.y+n.h);cairo_line_to(c,x+10+n.h*.58,n.y);cairo_line_to(c,x+n.h*.58,n.y);cairo_close_path(c);cairo_fill(c);
     }cairo_restore(c);
    }
   };
   const auto paint=[&](const Node& n){
    if(chrome_only&&(n.kind==Kind::arc||n.reader_row>=0||
       n.y+n.h<=first_y||n.y>=last_y))return;
    if(!n.clip_h){paint_node(n);return;}
    cairo_save(c);cairo_rectangle(c,0,n.clip_y,eye_size,n.clip_h);cairo_clip(c);
    paint_node(n);cairo_restore(c);
   };
   // The flat page background stays behind context. Opaque chrome,
   // including the report command, stays in front of it.
   auto paint_page=[&](const std::vector<Node>& nodes){
    if(nodes.empty())return;
    const bool has_context=std::any_of(nodes.begin(),nodes.end(),[](const Node& n){return n.kind==Kind::reader_context;});
    if(has_context){
     paint(nodes.front());
     paint_reader_context(c,nodes,depth,eye,first_y,last_y);
    }
    for(auto it=nodes.begin()+has_context;it!=nodes.end();++it)paint(*it);
   };
   auto layer=[&](const std::vector<Node>& nodes,double opacity,double x,double y,double w,double h){
    if(opacity<=0 || nodes.empty())return;
    if(opacity>=1){paint_page(nodes);return;}
    if(std::any_of(nodes.begin(),nodes.end(),[](const Node& n){return n.kind==Kind::reader_context;})){
     x=0;y=0;w=eye_size;h=eye_size;
    }
    cairo_save(c);cairo_rectangle(c,x,y,w,h);cairo_clip(c);cairo_push_group(c);
    paint_page(nodes);
    cairo_pop_group_to_source(c);cairo_paint_with_alpha(c,opacity);cairo_restore(c);
   };
   paint_page(base_previous);
   if(app.page=="LOG_VIEW"||app.report_detail())layer(base_current,page_alpha,32,408,2096,1344);
   else layer(base_current,page_alpha,328,408,1504,1344);
   // Menus only cover the content area; avoid allocating a full-eye blend for each menu frame.
   const auto popup_layer=[&](const std::vector<Node>& list,double opacity) {
    const bool dim=!list.empty()&&list.front().w==eye_size;
    if(dim)layer(list,opacity,0,0,eye_size,eye_size);
    else layer(list,opacity,440,640,850,980);
   };
   popup_layer(overlay_previous,1-layer_alpha);
   popup_layer(overlay_current,layer_alpha);
   for(std::size_t j=app.status_begin;j<app.nodes.size();++j)paint(app.nodes[j]);
   require(cairo_status(c)==CAIRO_STATUS_SUCCESS,"绘制界面");cairo_destroy(c);cairo_surface_flush(s);cairo_surface_destroy(s);
  }
  };
  if(chrome_only){
   const auto viewport=reader_viewport(app.report_detail());
   draw_band(0,viewport.top);
   draw_band(viewport.bottom(),eye_size);
  }else draw_band(0,eye_size);
 }
};
struct Buffer {
 int fd=-1; std::uint32_t handle=0,fb=0,pitch=0; std::uint64_t size=0; unsigned char* data=nullptr;
 void create(int device,int w,int h) {
  fd=device; drm_mode_create_dumb request{};request.width=w;request.height=h;request.bpp=32;
  require(drmIoctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&request)==0,"创建显示缓冲");
  handle=request.handle;pitch=request.pitch;size=request.size;
  std::uint32_t handles[4]{handle},pitches[4]{pitch},offsets[4]{};
  require(drmModeAddFB2(fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&fb,0)==0,"注册显示缓冲");
  drm_mode_map_dumb map{};map.handle=handle;require(drmIoctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&map)==0,"映射显示缓冲");
  data=static_cast<unsigned char*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,map.offset));
  require(data!=MAP_FAILED,"映射显示内存");
 }
 ~Buffer() { if(data&&data!=MAP_FAILED)munmap(data,size);if(fb)drmModeRmFB(fd,fb);if(handle){drm_mode_destroy_dumb d{};d.handle=handle;drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);} }
};
class Display {
 std::uint32_t connector=0,crtc=0,plane=0,mode_blob=0; bool committed=false;
 bool flip_ready=false;
 std::map<std::pair<std::uint32_t,std::string>,std::uint32_t> properties;
 void add(drmModeAtomicReq* req,std::uint32_t object,std::uint32_t type,const char* name,std::uint64_t value) {
  auto key=std::make_pair(object,std::string(name)); auto id=properties[key];
  if(!id) {
   auto* ps=drmModeObjectGetProperties(fd,object,type);require(ps,"读取显示属性");
   for(std::uint32_t i=0;i<ps->count_props;i++) { auto* p=drmModeGetProperty(fd,ps->props[i]);if(p){if(name==std::string(p->name))id=p->prop_id;drmModeFreeProperty(p);} }
   drmModeFreeObjectProperties(ps);properties[key]=id;
  }
  require(id&&drmModeAtomicAddProperty(req,object,id,value)>=0,std::string("设置显示属性 ")+name);
 }
public:
 int fd=-1;std::array<Buffer,2> buffers;int current=-1;drmModeModeInfo mode{};
 unsigned flip_sequence=0;
 double flip_at=0;
 Display() {
  for(auto& entry:fs::directory_iterator("/dev/dri")) {
   if(!entry.path().filename().string().starts_with("card"))continue;
   int candidate=open(entry.path().c_str(),O_RDWR|O_CLOEXEC);if(candidate<0)continue;
   auto* version=drmGetVersion(candidate);bool msm=version&&std::string(version->name,version->name_len)=="msm";if(version)drmFreeVersion(version);
   if(msm){fd=candidate;break;}close(candidate);
  }
  require(fd>=0,"打开 MSM 显示设备");require(drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)==0,"启用显示平面");
  require(drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1)==0,"启用原子显示");require(drmSetMaster(fd)==0,"获得显示权限");
  auto* resources=drmModeGetResources(fd);require(resources,"读取显示资源");
  int crtc_index=-1;
  for(int i=0;i<resources->count_connectors&&!connector;i++) {
   auto* c=drmModeGetConnector(fd,resources->connectors[i]);if(!c)continue;
   for(int j=0;c->connection==DRM_MODE_CONNECTED&&j<c->count_modes;j++) {
    const auto& candidate_mode=c->modes[j];
    int hz=candidate_mode.vrefresh?candidate_mode.vrefresh:static_cast<int>(std::lround(candidate_mode.clock*1000./(candidate_mode.htotal*candidate_mode.vtotal)));
    if(candidate_mode.hdisplay!=canvas_width||candidate_mode.vdisplay!=eye_size||hz!=90)continue;
    for(int k=0;k<c->count_encoders&&!connector;k++) {
     auto* enc=drmModeGetEncoder(fd,c->encoders[k]);if(!enc)continue;
     for(int n=0;n<resources->count_crtcs;n++) if(enc->possible_crtcs&(1u<<n)) {connector=c->connector_id;crtc=resources->crtcs[n];crtc_index=n;mode=c->modes[j];break;}
     drmModeFreeEncoder(enc);
    }
   }drmModeFreeConnector(c);
  }drmModeFreeResources(resources);require(connector&&crtc,"找到原生 90 Hz 双眼模式");
  auto* planes=drmModeGetPlaneResources(fd);require(planes,"读取显示平面");
  for(std::uint32_t i=0;i<planes->count_planes&&!plane;i++) {
   auto* p=drmModeGetPlane(fd,planes->planes[i]);if(!p)continue;
   bool xrgb=false;for(std::uint32_t j=0;j<p->count_formats;j++)xrgb|=p->formats[j]==DRM_FORMAT_XRGB8888;
   if(xrgb&&(p->possible_crtcs&(1u<<crtc_index))) {
    auto* props=drmModeObjectGetProperties(fd,p->plane_id,DRM_MODE_OBJECT_PLANE);
    if(props)for(std::uint32_t j=0;j<props->count_props;j++){auto* prop=drmModeGetProperty(fd,props->props[j]);if(prop){if(std::string(prop->name)=="type"&&props->prop_values[j]==DRM_PLANE_TYPE_PRIMARY)plane=p->plane_id;drmModeFreeProperty(prop);}}
    if(props)drmModeFreeObjectProperties(props);
   }drmModeFreePlane(p);
  }drmModeFreePlaneResources(planes);require(plane,"找到原生主显示平面");
  require(drmModeCreatePropertyBlob(fd,&mode,sizeof(mode),&mode_blob)==0,"创建显示模式");
  for(auto& b:buffers)b.create(fd,canvas_width,eye_size);
 }
 void present(int index) {
  auto* req=drmModeAtomicAlloc();require(req,"创建显示提交");
  try {
   if(!committed) {
    add(req,connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",crtc);
    add(req,crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",mode_blob);add(req,crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",1);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_ID",crtc);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_X",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_Y",0);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_W",std::uint64_t(canvas_width)<<16);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_H",std::uint64_t(eye_size)<<16);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_X",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0);
    add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_W",canvas_width);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_H",eye_size);
   }add(req,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",buffers[index].fb);
   const bool track_flip=committed;
   flip_ready=false;
   require(drmModeAtomicCommit(fd,req,track_flip?DRM_MODE_PAGE_FLIP_EVENT:DRM_MODE_ATOMIC_ALLOW_MODESET,this)==0,
           "提交原生显示帧");
   if(track_flip){
    drmEventContext context{};context.version=2;
    context.page_flip_handler=[](int,unsigned sequence,unsigned sec,unsigned usec,void* user){
     auto& display=*static_cast<Display*>(user);
     display.flip_sequence=sequence;display.flip_at=sec+usec*1e-6;display.flip_ready=true;
    };
    while(!flip_ready){
     pollfd event{fd,POLLIN,0};
     const int rc=poll(&event,1,250);
     if(rc<0&&errno==EINTR)continue;
     require(rc>0&&(event.revents&POLLIN),"等待显示翻转");
     require(drmHandleEvent(fd,&context)==0,"读取显示翻转");
    }
   }
   committed=true;current=index;drmModeAtomicFree(req);
  }catch(...){drmModeAtomicFree(req);throw;}
 }
 ~Display() {
  if(mode_blob)drmModeDestroyPropertyBlob(fd,mode_blob);
  // Buffers must be removed while their device descriptor is still open.
  for(auto& b:buffers) {if(b.data&&b.data!=MAP_FAILED)munmap(b.data,b.size);b.data=nullptr;if(b.fb)drmModeRmFB(fd,b.fb);b.fb=0;if(b.handle){drm_mode_destroy_dumb d{};d.handle=b.handle;drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);b.handle=0;}}
  if(fd>=0){drmDropMaster(fd);close(fd);}
 }
};
struct Input {
 int fd=-1;std::string name;std::array<bool,KEY_MAX+1> down{};std::array<double,KEY_MAX+1> began{},repeated{};
 std::array<bool,KEY_MAX+1> reader_press{};
 bool lost=false;
 ~Input(){if(fd>=0)close(fd);}
 bool event(const input_event& e,const std::function<void(int)>& dispatch,const std::function<void(bool)>& held={},
            const std::function<bool(int,int,double)>& reader={},const std::function<bool()>& cancel={},
            bool reader_mode=false) {
  if(e.type==EV_SYN&&e.code==SYN_DROPPED){
   lost=true;down.fill(false);reader_press.fill(false);
   if(held)held(false);
   const bool moved=cancel&&cancel();return bool(held)||moved;
  }
  if(lost){if(e.type==EV_SYN&&e.code==SYN_REPORT)lost=false;return false;}
  if(e.type!=EV_KEY||(e.code!=KEY_POWER&&e.code!=KEY_VOLUMEUP&&e.code!=KEY_VOLUMEDOWN))return false;
  double now=monotonic_seconds();
  if(e.value==1&&!down[e.code]){
   down[e.code]=true;began[e.code]=repeated[e.code]=now;
   if(e.code!=KEY_POWER){
    reader_press[e.code]=reader_mode&&bool(reader);
    if(reader_press[e.code])return reader(e.code,1,now);
    dispatch(e.code);return true;
   }
   if(held){held(true);return true;}
  }
  if(e.value==0){
   const bool was_down=std::exchange(down[e.code],false);
   if(e.code==KEY_POWER){
    const bool valid=was_down&&now-began[e.code]<.8;
    if(valid)dispatch(e.code);
    if(held)held(false);
    return valid||bool(held);
   }
   if(std::exchange(reader_press[e.code],false)&&reader&&was_down)return reader(e.code,0,now);
  }
  if(e.value==2&&e.code!=KEY_POWER&&!reader_press[e.code]&&down[e.code]&&now-began[e.code]>=.5&&now-repeated[e.code]>=.1){repeated[e.code]=now;dispatch(e.code);return true;}
  return false;
 }
};
bool bit(const std::array<unsigned long,(KEY_MAX+64)/64>& bits,int key){return bits[key/(sizeof(long)*8)]&(1ul<<(key%(sizeof(long)*8)));}
void discover(std::vector<std::unique_ptr<Input>>& inputs,bool inventory=false) {
 if(!fs::exists("/dev/input"))return;
 for(auto& e:fs::directory_iterator("/dev/input")) {
  if(!e.path().filename().string().starts_with("event"))continue;
  const auto path=e.path().string();bool exists=false;for(auto& i:inputs)exists|=i->name==path;if(exists)continue;
  int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
  if(fd<0){if(inventory)std::cout<<path<<" open_errno="<<errno<<"\n";continue;}
  std::array<unsigned long,(KEY_MAX+64)/64> keys{};
  int rc=ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys.data());
  if(inventory) {
   std::array<char,256> name{};ioctl(fd,EVIOCGNAME(name.size()),name.data());
   std::cout<<path<<" name="<<name.data()<<" ioctl_bytes="<<rc<<" power="<<bit(keys,KEY_POWER)
    <<" volume_up="<<bit(keys,KEY_VOLUMEUP)<<" volume_down="<<bit(keys,KEY_VOLUMEDOWN)<<"\n";
  }
  if(rc<0||(!bit(keys,KEY_POWER)&&!bit(keys,KEY_VOLUMEUP)&&!bit(keys,KEY_VOLUMEDOWN))){close(fd);continue;}
  int clock=CLOCK_MONOTONIC;ioctl(fd,EVIOCSCLOCKID,&clock);
  auto i=std::make_unique<Input>();i->fd=fd;i->name=path;inputs.push_back(std::move(i));
  std::cout<<"输入设备已连接 "<<path<<"\n";
 }
}
struct Snapshot {std::vector<unsigned char> bytes;int pitch;std::uint64_t frame;std::string page,boot,build;double capture;};
void png(const fs::path& path,const Snapshot& shot,int x,int width) {
 auto* file=fopen(path.c_str(),"wb");require(file,"写入截图");
 auto* p=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);auto* info=p?png_create_info_struct(p):nullptr;
 if(!p||!info){fclose(file);throw std::runtime_error("创建 PNG 编码器失败");}
 if(setjmp(png_jmpbuf(p))){png_destroy_write_struct(&p,&info);fclose(file);throw std::runtime_error("PNG 编码失败");}
 png_init_io(p,file);png_set_IHDR(p,info,width,eye_size,8,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
 png_set_compression_level(p,3);png_write_info(p,info);
 std::vector<unsigned char> row(width*3);
 for(int y=0;y<eye_size;y++) {
  auto* input=shot.bytes.data()+y*shot.pitch+x*4;
  for(int i=0;i<width;i++){row[i*3]=input[i*4+2];row[i*3+1]=input[i*4+1];row[i*3+2]=input[i*4];}
  png_write_row(p,row.data());
 }png_write_end(p,info);png_destroy_write_struct(&p,&info);require(fflush(file)==0&&fsync(fileno(file))==0,"同步截图");fclose(file);
}
std::string save_snapshot(Snapshot shot) {
 auto folder=fs::path("/run/overpressure/screenshots")/("frame-"+std::to_string(shot.frame)+"-"+std::to_string(static_cast<long long>(shot.capture*1e6)));
 fs::create_directories(folder);fs::permissions(folder,fs::perms::owner_all);
 png(folder/"双眼.png",shot,0,canvas_width);png(folder/"左眼.png",shot,0,eye_size);png(folder/"右眼.png",shot,eye_size,eye_size);
 auto* j=json_object_new_object();
 auto string=[&](const char* key,const std::string& value){json_object_object_add(j,key,json_object_new_string(value.c_str()));};
 string("directory",folder.string());string("page",shot.page);string("boot_id",shot.boot);string("build",shot.build);
 json_object_object_add(j,"frame",json_object_new_int64(shot.frame));json_object_object_add(j,"width",json_object_new_int(canvas_width));
 json_object_object_add(j,"height",json_object_new_int(eye_size));json_object_object_add(j,"pitch",json_object_new_int(shot.pitch));
 json_object_object_add(j,"capture_monotonic_seconds",json_object_new_double(shot.capture));
 json_object_object_add(j,"atomic_commit_completed",json_object_new_boolean(true));json_object_object_add(j,"physical_panel_verified",json_object_new_boolean(false));
 std::string result=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PRETTY);json_object_put(j);
 std::ofstream(folder/"截图信息.json")<<result<<"\n";return result;
}
int screenshot_client(int delay_ms=0) {
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);require(fd>=0,"打开截图通道");
 sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,"/run/overpressure/ui.sock");
 require(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"连接界面截图服务");
 if(delay_ms)usleep(delay_ms*1000);
 require(send(fd,"capture",7,MSG_NOSIGNAL)==7,"请求截图");pollfd p{fd,POLLIN,0};require(poll(&p,1,30000)>0,"等待截图");
 // SOCK_SEQPACKET discards the remainder of a packet when its receive buffer is too small.
 // Build metadata grows as image sources are added, so size the receive to the whole packet.
 constexpr ssize_t max_receipt=128*1024;
 const auto size=recv(fd,nullptr,0,MSG_PEEK|MSG_TRUNC);
 if(size<=0||size>max_receipt){close(fd);throw std::runtime_error("Invalid screenshot receipt size");}
 std::string buffer(static_cast<std::size_t>(size),'\0');
 const auto received=recv(fd,buffer.data(),buffer.size(),MSG_TRUNC);
 close(fd);require(received==size,"Incomplete screenshot receipt");
 std::cout<<buffer<<'\n';return buffer.starts_with("{")?0:1;
}
int server_socket() {
 fs::create_directories("/run/overpressure");int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);require(fd>=0,"创建截图服务");
 sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,"/run/overpressure/ui.sock");unlink(address.sun_path);
 require(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"绑定截图服务");chmod(address.sun_path,0600);require(listen(fd,2)==0,"启动截图服务");return fd;
}
void write_frame_metadata(const App& app,double draw,double commit,const std::optional<OpticsRing>& ring,
                          const Display& display,bool body_only) {
 auto* j=json_object_new_object();
 json_object_object_add(j,"page",json_object_new_string(ring?"OPTICS":app.page.c_str()));json_object_object_add(j,"focus",json_object_new_int(app.focus));
 json_object_object_add(j,"frame",json_object_new_int64(app.frame));json_object_object_add(j,"draw_ms",json_object_new_double(draw*1000));
 json_object_object_add(j,"commit_ms",json_object_new_double(commit*1000));json_object_object_add(j,"locale",json_object_new_string(app.backend.preferences.locale.c_str()));
 auto* optics=ring?json_object_new_object():nullptr;
 if(ring){
  json_object_object_add(optics,"id",json_object_new_string(ring->id.c_str()));
  json_object_object_add(optics,"diameter_px",json_object_new_int(ring->diameter));
  json_object_object_add(optics,"stroke_px",json_object_new_int(ring->stroke));
  json_object_object_add(optics,"x",json_object_new_int(ring->x));
  json_object_object_add(optics,"y",json_object_new_int(ring->y));
 }
 json_object_object_add(j,"optics_ring",optics);
 json_object_object_add(j,"log_width_px",json_object_new_int(app.log_width.viewport));
 if(!ring&&app.page=="LOG_VIEW"){
  constexpr auto viewport=reader_viewport(false);
  json_object_object_add(j,"log_text_width_px",json_object_new_int(app.log_width.text()));
  json_object_object_add(j,"log_top_line",json_object_new_int(app.log_top));
  json_object_object_add(j,"log_selected_line",app.log_back?nullptr:json_object_new_int(app.log_cursor));
  json_object_object_add(j,"log_total_lines",json_object_new_int(app.log_layout.row_count));
  json_object_object_add(j,"log_visible_lines",json_object_new_int(static_cast<int>(std::count_if(
    app.nodes.begin(),app.nodes.end(),[&](const Node& n){
     return n.prewrapped&&n.y>=viewport.top&&n.y+n.h<=viewport.bottom();
    }))));
 }
 if(!ring&&app.reader_view()){
  json_object_object_add(j,"reader_offset_px",json_object_new_int(app.reader_scroll.offset_px));
  json_object_object_add(j,"reader_visual_top_px",json_object_new_int64(app.reader_visual_top_px()));
  json_object_object_add(j,"reader_coasting",
     json_object_new_boolean(app.reader_scroll.coasting(app.animation_phase)));
  json_object_object_add(j,"reader_catching_up",json_object_new_boolean(app.reader_scroll.catch_rows>0));
  json_object_object_add(j,"reader_velocity_rows_s",json_object_new_double(app.reduced_motion?0:
     -app.reader_scroll.velocity_at(app.animation_phase)/log_line_px));
 }
 if(display.flip_at>0){
  json_object_object_add(j,"flip_sequence",json_object_new_int64(display.flip_sequence));
  json_object_object_add(j,"flip_monotonic_s",json_object_new_double(display.flip_at));
 }
 json_object_object_add(j,"reader_body_only",json_object_new_boolean(body_only));
 if(!ring&&app.report_detail()){
  json_object_object_add(j,"report_top_line",json_object_new_int(app.report.top_line));
  json_object_object_add(j,"report_total_lines",json_object_new_int(static_cast<int>(app.report.lines.size())));
  json_object_object_add(j,"report_visible_lines",json_object_new_int(std::min(report_view_lines,
      static_cast<int>(app.report.lines.size())-app.report.top_line)));
 }
 json_object_object_add(j,"language_durable",json_object_new_boolean(app.backend.preferences.durable));
 json_object_object_add(j,"language_selected",json_object_new_boolean(app.backend.preferences.language_selected));
 json_object_object_add(j,"install_preview",json_object_new_boolean(app.backend.install_preview));
 json_object_object_add(j,"install_live_log",json_object_new_boolean(app.page=="LOG_VIEW"&&app.log_source=="install-live"));
 json_object_object_add(j,"live_log_event_driven",json_object_new_boolean(app.live_log_events));
 json_object_object_add(j,"follow_install_log",json_object_new_boolean(app.page=="LOG_VIEW"&&app.follow_install_log));
 json_object_object_add(j,"task_stage_count",json_object_new_int(app.backend.task.stages.size()));
 json_object_object_add(j,"task_active_stage",json_object_new_int(app.backend.task.active));
 json_object_object_add(j,"task_status",json_object_new_string(app.backend.task.status.c_str()));
 if(app.backend.install_preview){
  json_object_object_add(j,"preview_step",json_object_new_int(app.backend.preview_step));
  const auto& task=app.backend.task;
  json_object_object_add(j,"preview_route",json_object_new_string(task.id.c_str()));
  if(task.active>=0&&static_cast<std::size_t>(task.active)<task.stages.size()){
   const auto& stage=task.stages[task.active];
   json_object_object_add(j,"preview_stage",json_object_new_string(stage.id.c_str()));
   const auto active=std::find_if(stage.substeps.begin(),stage.substeps.end(),[](const Substep& sub){
    return sub.state=="running"||sub.state=="waiting"||sub.state=="error";});
   if(active!=stage.substeps.end())json_object_object_add(j,"preview_substep",json_object_new_string(active->id.c_str()));
  }
 }
 const auto& telemetry=app.backend.telemetry;
 json_object_object_add(j,"time_valid",json_object_new_boolean(telemetry.time_valid));
 json_object_object_add(j,"clock",json_object_new_string(telemetry.clock.c_str()));
 json_object_object_add(j,"date",json_object_new_string(telemetry.date.c_str()));
 json_object_object_add(j,"timezone",json_object_new_string(telemetry.timezone.c_str()));
 json_object_object_add(j,"time_source",json_object_new_string(telemetry.time_source.c_str()));
 json_object_object_add(j,"ncm_state",json_object_new_string(telemetry.ncm_state.c_str()));
 json_object_object_add(j,"ncm_interface",json_object_new_string(telemetry.ncm_interface.c_str()));
 json_object_object_add(j,"upload_bps",telemetry.upload_bps?json_object_new_double(*telemetry.upload_bps):nullptr);
 json_object_object_add(j,"download_bps",telemetry.download_bps?json_object_new_double(*telemetry.download_bps):nullptr);
 json_object_object_add(j,"network_sample_seconds",telemetry.network_sample_seconds?
  json_object_new_double(*telemetry.network_sample_seconds):nullptr);
 json_object_object_add(j,"network_rx_bytes",telemetry.network_rx_bytes?
  json_object_new_uint64(*telemetry.network_rx_bytes):nullptr);
 json_object_object_add(j,"network_tx_bytes",telemetry.network_tx_bytes?
  json_object_new_uint64(*telemetry.network_tx_bytes):nullptr);
 std::ofstream("/run/overpressure/ui-frame.json.tmp")<<json_object_to_json_string_ext(j,JSON_C_TO_STRING_PRETTY)<<"\n";
 rename("/run/overpressure/ui-frame.json.tmp","/run/overpressure/ui-frame.json");json_object_put(j);
}
bool preview_requested() {
 struct stat dir{},marker{};
 if(lstat("/run/overpressure/ui-install-preview/active",&marker)!=0)return false;
 require(lstat("/run/overpressure/ui-install-preview",&dir)==0&&S_ISDIR(dir.st_mode)&&dir.st_uid==geteuid()&&
         !(dir.st_mode&0077)&&S_ISREG(marker.st_mode)&&marker.st_uid==geteuid()&&
         !(marker.st_mode&0077),"验证安装模拟的专用内存目录");
 return true;
}
struct PreviewTimeline {
 struct Frame {int ms;bool boundary;};
 std::vector<Frame> frames;
 int frame=0;
 double next=0;
 bool playing=false;

 explicit PreviewTimeline(bool enabled) {
  if(!enabled)return;
  constexpr const char* dir="/run/overpressure/ui-install-preview/";
  std::ifstream plan(std::string(dir)+"timeline");
  if(!plan)return; // Keep the existing short RAM preview available.
  int ms,boundary;
  while(plan>>ms>>boundary){
   require(frames.size()<4096&&ms>=0&&ms<=6000&&(boundary==0||boundary==1),"验证模拟时间线");
   frames.push_back({ms,boundary!=0});
  }
  std::ifstream cursor(std::string(dir)+"initial");
  require(plan.eof()&&(cursor>>frame)&&frame>=0&&static_cast<std::size_t>(frame)<frames.size(),"验证模拟起点");
 }
 bool enabled() const {return !frames.empty();}
 bool key(int code,double now) {
  if(code==KEY_POWER){
   if(playing){playing=false;return true;}
   if(frames[frame].ms==0){
    if(frame+1==static_cast<int>(frames.size()))return false;
    ++frame;
   }
   playing=true;
   next=now+frames[frame].ms*.001;
   return true;
  }
  playing=false;
  const int direction=code==KEY_VOLUMEUP?1:-1;
  for(int candidate=frame+direction;candidate>=0&&candidate<static_cast<int>(frames.size());candidate+=direction){
   if(frames[candidate].boundary){
    frame=candidate;
    return true;
   }
  }
  return false;
 }
 bool tick(double now) {
  if(!playing||now<next)return false;
  if(frames[frame].ms==0||frame+1==static_cast<int>(frames.size())){playing=false;return false;}
  ++frame;
  next=now+frames[frame].ms*.001;
  if(frames[frame].ms==0)playing=false;
  return true;
 }
};
int run(bool install_preview) {
 signal(SIGINT,stop);signal(SIGTERM,stop);
 std::cout.setf(std::ios::unitbuf);
 int preview_lock=-1;
 if(install_preview){
  // Actual installer clients cannot acquire the writer lock throughout the demo.
  preview_lock=open("/run/overpressure/install.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
  require(preview_lock>=0&&flock(preview_lock,LOCK_EX|LOCK_NB)==0,"安装预览需要空闲的安装锁");
 }
 PreviewTimeline replay(install_preview);
 App app(install_preview,true);app.reduced_motion=std::getenv("P4_REDUCED_MOTION")!=nullptr;
 app.backend.manual_preview=replay.enabled();
 startup_mark("initializing DRM");
 Display display;
 startup_mark("native display mode ready");
 Painter painter("/usr/share/pico4/recovery/fonts","/usr/share/pico4/recovery/breeze/atlas.png");
 painter.bind_log_metrics(app);
 // Draw once before storage probes, settings, or input discovery can block.
 // Keep that frame on screen until normal startup can draw its first page.
 app.compose();
 auto& startup=display.buffers[0];
 painter.scene(startup.data,startup.pitch,app);
 display.present(0);
 startup_mark("startup frame committed; loading settings");
 app.finish_startup();
 startup_mark("settings and system label checked");
 int server=server_socket();
 int optics_watch=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
 if(optics_watch>=0&&inotify_add_watch(optics_watch,"/run/overpressure",
        IN_MODIFY|IN_CLOSE_WRITE|IN_CREATE|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE|IN_DELETE_SELF|IN_MOVE_SELF)<0){
  close(optics_watch);optics_watch=-1;
 }
 app.live_log_events=optics_watch>=0&&!install_preview;
 bool task_changed=false;
 std::optional<OpticsRing> ring;
 std::string dismissed;
 bool optics_reload=true,log_width_reload=true;
 double optics_checked=0;
 std::vector<std::unique_ptr<Input>> inputs;discover(inputs);
 Input* reader_owner=nullptr;
 const auto cancel_reader=[&]{
  reader_owner=nullptr;
  return app.cancel_reader_scroll();
 };
 struct CaptureRequest {int fd;double accepted;};
 std::vector<CaptureRequest> requests;
 int shot_client=-1;std::future<std::string> shot;double refreshed=0,discovered=monotonic_seconds(),last_frame=0;
 bool dirty=true,body_update=false,inventory_deferred=false;
 const double preview_started=monotonic_seconds();
 while(!stopping) {
  double now=monotonic_seconds();
  const bool replay_changed=replay.tick(now)||(replay.enabled()&&app.backend.preview_step!=replay.frame);
  if(replay_changed)app.live_log_dirty=true;
  if(log_width_reload||(optics_watch<0&&now-optics_checked>=.25)){
   const int width=parse_log_width(app.backend.read(log_width_path,16));
   if(width!=app.log_width.viewport){app.log_width.viewport=width;cancel_reader();dirty=true;}
   log_width_reload=false;
  }
  if(optics_reload||(optics_watch<0&&now-optics_checked>=.25)){
   const auto requested=parse_optics(app.backend.read(optics_path,97),eye_size);
   auto next=requested;
   const int boot=boot_seconds();
   if(next&&(next->id==dismissed||!boot||next->expires<=boot||next->expires-boot>1800))next.reset();
   if(next!=ring){
    ring=std::move(next);cancel_reader();dirty=true;
    app.power_held=false;app.held_power.clear();
   }
   optics_reload=false;optics_checked=now;
  }
  if(ring&&ring->expires<=boot_seconds()){ring.reset();cancel_reader();dirty=true;}
  const bool in_reader=app.frame>0&&app.reader_view()&&!ring;
  if(replay_changed||now-refreshed>=1||(!in_reader&&inventory_deferred)) {
   // Telemetry and task status remain live; only readers omit unused disk inventory.
   if(replay.enabled())app.backend.preview_step=replay.frame;
   else if(install_preview)app.backend.preview_step=now-preview_started<12?-1:
        std::min(9,static_cast<int>((now-preview_started-12)/8));
   app.backend.refresh(!in_reader);
   if(!ring&&app.observe_install_task())cancel_reader();
   if(!install_preview&&!app.frame&&app.page=="HOME"&&!app.backend.task.id.empty()){app.page="CURRENT_TASK";app.focus=1;}
   refreshed=now;dirty=true;
   task_changed=false;
  }else if(task_changed){
   app.backend.refresh_task();
   if(!ring&&app.observe_install_task()){cancel_reader();dirty=true;}
   task_changed=false;
   dirty|=!in_reader||log_progress::watches_task_log(app);
  }
  if(now-discovered>=2) {discover(inputs);discovered=now;}
  if(shot.valid()&&shot.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
   std::string result;try{result=shot.get();}catch(const std::exception& e){result=e.what();}
   send(shot_client,result.data(),result.size(),MSG_NOSIGNAL);close(shot_client);shot_client=-1;
  }
  const auto progress_moving=[&]{return !ring&&!app.reduced_motion&&painter.progress_moving();};
  const bool legacy_tick=!ring&&painter.legacy_progress_moving()&&now-last_frame>=.1;
  if(dirty||body_update||progress_moving()||legacy_tick) {
   app.animation_phase=now;
   if(dirty||body_update)app.compose();
   int next=display.current==0?1:0;
   const bool has_rows=app.report_detail()?!app.report.lines.empty():app.log_layout.row_count>0;
   const bool fast_reader=!ring&&has_rows&&painter.stable_reader(app);
   auto before=monotonic_seconds();
   auto& buffer=display.buffers[next];
   if(ring)draw_optics(buffer.data,buffer.pitch,*ring);
   else if(fast_reader)painter.reader_scene(buffer.data,buffer.pitch,app);
   else painter.scene(buffer.data,buffer.pitch,app);
   auto drawn=monotonic_seconds();display.present(next);auto committed=monotonic_seconds();
   ++app.frame;
   if(app.frame==1)startup_mark("first interactive frame committed");
   write_frame_metadata(app,drawn-before,committed-drawn,ring,display,false);
   dirty=false;body_update=false;last_frame=now;
  }
  if(!ring&&painter.animating())dirty=true;
  if(app.language_save.valid()&&now-last_frame>.05)dirty=true;
  std::vector<pollfd> pollers{{server,POLLIN,0}};for(auto& i:inputs)pollers.push_back({i->fd,POLLIN,0});
  const auto optics_index=pollers.size();
  if(optics_watch>=0)pollers.push_back({optics_watch,POLLIN,0});
  for(auto& request:requests)pollers.push_back({request.fd,POLLIN,0});
  const double wait_at=monotonic_seconds();
  const auto& scroll=app.reader_scroll;
  const bool moving=!app.reduced_motion&&scroll.animating(wait_at);
  // A moving reader commits against each vblank; never sleep between frames.
  const int wait_ms=std::min(moving||progress_moving()?0:20,scroll.next_step>0?
    static_cast<int>(std::ceil(std::clamp((scroll.next_step-wait_at)*1000,0.,20.))):20);
  int rc=poll(pollers.data(),pollers.size(),wait_ms);if(rc<0){if(errno==EINTR)continue;require(false,"读取按键");}
  if(optics_watch>=0&&pollers[optics_index].revents){
   alignas(inotify_event) char events[4096];
   const auto count=read(optics_watch,events,sizeof(events));
   if(count<0&&errno!=EAGAIN&&errno!=EINTR){
    close(optics_watch);optics_watch=-1;app.live_log_events=false;app.live_log_dirty=true;dirty=true;
   }
   for(std::size_t offset=0;count>0&&offset+sizeof(inotify_event)<=static_cast<std::size_t>(count);){
    const auto* event=reinterpret_cast<const inotify_event*>(events+offset);
    const auto name=event->len?std::string_view(event->name):std::string_view{};
    if((event->mask&IN_Q_OVERFLOW)||name=="optics-ring")optics_reload=true;
    if((event->mask&IN_Q_OVERFLOW)||name=="log-width")log_width_reload=true;
    if(app.live_log_events){
     const bool overflow=event->mask&IN_Q_OVERFLOW;
     if(overflow||name=="task.json"||name=="install.lock"){
      task_changed=true;
     }
     if(overflow||name=="task.log"||name=="boot.log"){
      app.live_log_dirty=true;
      dirty|=app.page=="CURRENT_TASK"||(overflow&&app.page=="LOG_VIEW")||
       (name=="task.log"&&log_progress::watches_task_log(app))||
       (name=="boot.log"&&log_progress::watches_boot_log(app));
     }
     if(event->mask&(IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF)){
      app.live_log_events=false;app.live_log_dirty=true;dirty=true;
     }
    }
    offset+=sizeof(*event)+event->len;
   }
  }
  if(pollers[0].revents&POLLIN) {
   int client=accept4(server,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
   if(client>=0) {
    if(requests.size()<2)requests.push_back({client,monotonic_seconds()});
    else close(client);
   }
  }
  // accept() only means connected: the client may not have sent its packet
  // yet. Keep the nonblocking socket in the poll set instead of closing on
  // EAGAIN, which races the client's send() and reports Broken pipe.
  for(std::size_t i=0;i<requests.size();) {
    int client=requests[i].fd;
    char request[32]{};int n=recv(client,request,sizeof(request),MSG_DONTWAIT);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)&&
       monotonic_seconds()-requests[i].accepted<1) {++i;continue;}
    if(n==7&&std::string(request,n)=="capture"&&!shot.valid()) {
     auto& b=display.buffers[display.current];Snapshot snapshot;
     snapshot.bytes.assign(b.data,b.data+b.size);snapshot.pitch=b.pitch;snapshot.frame=app.frame;snapshot.page=ring?"OPTICS":app.page;
     snapshot.boot=app.backend.read("/proc/sys/kernel/random/boot_id");snapshot.build=app.backend.read("/etc/pico4/ui-build.json");snapshot.capture=monotonic_seconds();
     shot_client=client;shot=std::async(std::launch::async,save_snapshot,std::move(snapshot));
    }else {const char* error="截图服务仅接受 capture，且一次处理一张截图";send(client,error,std::strlen(error),MSG_NOSIGNAL);close(client);}
    requests.erase(requests.begin()+i);
  }
  for(std::size_t i=0;i<inputs.size();) {
   auto* input=inputs[i].get();
   const auto lost_reader=[&]{return reader_owner==input&&cancel_reader();};
   if(pollers[i+1].revents&(POLLHUP|POLLERR|POLLNVAL)) {
    dirty|=lost_reader();std::cout<<"输入设备已断开 "<<input->name<<"\n";
    inputs.erase(inputs.begin()+i);pollers.erase(pollers.begin()+i+1);continue;
   }
   if(pollers[i+1].revents&POLLIN) {
    input_event events[32];ssize_t n;
    while((n=read(inputs[i]->fd,events,sizeof(events)))>0) {
     for(std::size_t j=0;j<static_cast<std::size_t>(n)/sizeof(input_event);j++){
      const auto dispatch=[&](int code){
       if(replay.enabled()&&!ring){replay.key(code,monotonic_seconds());return;}
       if(ring){
        if(code==KEY_POWER&&app.power_held){dismissed=ring->id;ring.reset();}
        app.power_held=false;app.held_power.clear();
        return;
       }
        if(code!=KEY_POWER&&app.reader_view())return; // Driver repeat never stacks with the reader timer.
       app.compose();
       const bool eligible=code!=KEY_POWER||(app.power_held&&app.held_power==app.activation());
       app.power_held=false;app.held_power.clear();
       if(eligible)app.key(code);
      };
      dirty|=input->event(events[j],dispatch,[&](bool pressed){
       if(pressed)cancel_reader();
       if(ring){app.power_held=pressed;return;}
       if(pressed)app.compose();
       app.power_held=pressed;app.held_power=pressed?app.activation():std::string{};
       },[&](int code,int value,double when){
        if(value==0){
         if(reader_owner==input&&app.reader_scroll.key==code){
          app.end_reader_scroll(code,when);
         }
         return false;
        }
        app.power_held=false;app.held_power.clear();
        const bool was_back=app.page=="LOG_VIEW"&&app.log_back;
        const bool changed=app.begin_reader_scroll(code,when);
        if(app.reader_scroll.key)reader_owner=input;
        else if(!app.reader_scroll.coasting(when))reader_owner=nullptr;
        if(!changed)return false;
        if(was_back!=(app.page=="LOG_VIEW"&&app.log_back))return true;
        body_update=true;
        return false;
       },lost_reader,!ring&&!replay.enabled()&&app.reader_view());
     }
    }
    if(n==0||(n<0&&errno!=EAGAIN&&errno!=EINTR)){
     dirty|=lost_reader();
     inputs.erase(inputs.begin()+i);pollers.erase(pollers.begin()+i+1);continue;
    }
   }++i;
  }
  // Apply a timer step only after draining input. A queued release wins over
  // the deadline even when a KMS frame or a capture stalled the loop.
  if(!ring&&!optics_reload){
   const double sample=monotonic_seconds();
   body_update|=app.tick_reader_scroll(sample);
   body_update|=app.reader_view()&&!app.reduced_motion&&app.reader_scroll.animating(sample);
  }
  if(!app.reader_scroll.key&&!app.reader_scroll.coasting(monotonic_seconds()))reader_owner=nullptr;
 }
 if(shot.valid()){try{shot.get();}catch(...){}}if(shot_client>=0)close(shot_client);
 for(auto& request:requests)close(request.fd);
 if(optics_watch>=0)close(optics_watch);
 close(server);unlink("/run/overpressure/ui.sock");
 if(preview_lock>=0)close(preview_lock);
 return 0;
}
int render_gallery(const fs::path& output) {
 // Exercise the same compositor without changing the active KMS frame or
 // dispatching any action. These images are explicitly offline layout checks.
 fs::create_directories(output);fs::permissions(output,fs::perms::owner_all);
 App app;app.backend.refresh();app.backend.preferences.language_selected=true;
 app.depth.zero=true;app.draft=app.depth;
 Painter painter(std::getenv("P4_GALLERY_FONTS")?std::getenv("P4_GALLERY_FONTS"):"/usr/share/pico4/recovery/fonts",
   std::getenv("P4_GALLERY_BREEZE")?std::getenv("P4_GALLERY_BREEZE"):"/usr/share/pico4/recovery/breeze/atlas.png");
 painter.bind_log_metrics(app);
 Snapshot image;image.pitch=canvas_width*4;image.bytes.resize(image.pitch*eye_size);
 auto* report=json_object_new_object();auto* cases=json_object_new_array();
 json_object_object_add(report,"mode",json_object_new_string("offline_zero_disparity_layout"));
 json_object_object_add(report,"cases",cases);
 auto render=[&](const std::string& name) {
  app.compose();painter.scene(image.bytes.data(),image.pitch,app);
   png(output/(name+".png"),image,0,eye_size);
  auto* item=json_object_new_object();auto* labels=json_object_new_array();
  auto* single_line=json_object_new_array();
  auto* status_ink=json_object_new_array();
  auto* status_runs=json_object_new_array();
  json_object_object_add(item,"image",json_object_new_string((name+".png").c_str()));
  json_object_object_add(item,"labels",labels);
  json_object_object_add(item,"single_line",single_line);
  json_object_object_add(item,"status_ink",status_ink);
  json_object_object_add(item,"status_runs",status_runs);
  for(const auto& run:painter.status_runs) {
   auto* label=json_object_new_object();auto* bounds=json_object_new_array();auto* color=json_object_new_array();
   for(double value:{run.x,run.y,run.w,run.h})json_object_array_add(bounds,json_object_new_double(value));
   for(double value:{run.color.r,run.color.g,run.color.b})json_object_array_add(color,json_object_new_int(std::lround(value*255)));
   json_object_object_add(label,"bounds",bounds);json_object_object_add(label,"color",color);
   json_object_object_add(label,"value",json_object_new_string(run.value.c_str()));
   json_object_object_add(label,"target_height",json_object_new_double(run.target));
   json_object_object_add(label,"font_face",json_object_new_int(run.face));
   json_object_array_add(status_runs,label);
  }
  for(const auto& n:app.nodes)if(n.ink_height>0) {
   auto* label=json_object_new_object();auto* bounds=json_object_new_array();auto* color=json_object_new_array();
   double width=n.w,x=n.x;
   if(n.kind==Kind::icon&&n.value.starts_with("battery:")){
    const auto b=painter.icon_bounds(n);
    width=std::ceil(b[1])+1-std::floor(b[0]);x=std::floor(b[0]);
   }
   for(double value:{x,n.y,width,n.h})json_object_array_add(bounds,json_object_new_double(value));
   for(double value:{n.color.r,n.color.g,n.color.b})json_object_array_add(color,json_object_new_int(std::lround(value*255)));
   json_object_object_add(label,"bounds",bounds);json_object_object_add(label,"color",color);
   json_object_object_add(label,"value",json_object_new_string(n.value.c_str()));
   json_object_object_add(label,"target_height",json_object_new_double(n.ink_height));
   json_object_object_add(label,"previous_icon_gap",json_object_new_double(n.previous_icon_gap));
   json_object_array_add(status_ink,label);
  }
  for(const auto& n:app.nodes)if(n.kind==Kind::text&&
      (n.ink_height>0||n.y==1634||(n.y==516&&n.h==76&&n.size==52))) {
   const auto width=painter.line_width(n);
   require(width<=n.w,"单行标签宽度："+n.value);
   auto* label=json_object_new_object();
   json_object_object_add(label,"text",json_object_new_string(n.value.c_str()));
   json_object_object_add(label,"advance_px",json_object_new_double(width));
   json_object_object_add(label,"available_px",json_object_new_double(n.w));
   json_object_array_add(single_line,label);
  }
  for(const auto& n:app.nodes)if(n.kind==Kind::text&&n.center&&n.middle) {
   auto* label=json_object_new_object();auto* bounds=json_object_new_array();auto* color=json_object_new_array();
   for(double value:{n.x,n.y,n.w,n.h})json_object_array_add(bounds,json_object_new_double(value));
   for(double value:{n.color.r,n.color.g,n.color.b})json_object_array_add(color,json_object_new_int(std::lround(value*255)));
   json_object_object_add(label,"bounds",bounds);json_object_object_add(label,"color",color);
   json_object_object_add(label,"text",json_object_new_string(n.value.c_str()));
   json_object_object_add(label,"back",json_object_new_boolean(n.h==62));
   json_object_array_add(labels,label);
  }
  json_object_array_add(cases,item);
 };
 for(const auto* page:{"LANGUAGE","BOOT","BOOT_ERROR","CURRENT_TASK","HOME","STATUS",
  "HARDWARE","SERVICES","DEADLINE","HOST","MAINT","BUILD_MENU","STOCK_MENU",
  "RESTORE_MENU","INPUTS","TASK_DETAIL","REVIEW","CONFIRM","RUNNING","SUCCESS",
  "ERROR","UNAVAILABLE","STORAGE","PARTITION","HASH","MOUNT","DIAG","REPORT",
  "LOGS","LOG_VIEW","POWER","BOOT_MODE","DISPLAY","LAYER_EDIT","EXIT"}) {
  app.navigate(page);render(page);
 }
 // Synthetic states are confined to this offline model, never task-state files.
 app.backend.task={};app.page="CURRENT_TASK";app.focus=1;render("CURRENT_TASK_IDLE");
 app.backend.task.id="layout-fixture";app.backend.task.name="离线布局样例";
 app.backend.task.status="done";app.backend.task.readonly_restored=true;
 render("CURRENT_TASK_DONE");
 app.backend.task.status="error";app.backend.task.error="离线失败状态布局样例";
 app.page="ERROR";render("ERROR_TWO_BUTTONS");
 for(const auto* state:{"disabled","down","disconnected","connecting","no-address","connected","suspended","unknown"}) {
  app.backend.telemetry.ncm_state=state;app.page="HOME";
  app.backend.telemetry.upload_bps=1023.0*1024;
  app.backend.telemetry.download_bps=99.9*1024*1024;
  render("NCM_"+std::string(state));
 }
 app.backend.task.stages={{"输入检查","done",{}},{"系统镜像","running",.375},{"启动","pending",{}}};
 app.backend.task.status="running";app.backend.task.active=1;app.page="LOG_VIEW";
 render("LOG_VIEW_RUNNING");
 app.backend.task.stages[1].state="error";app.backend.task.status="error";
 render("LOG_VIEW_ERROR");
 app.page="HOME";app.backend.telemetry.upload_bps=1023.0*1024;
 app.backend.telemetry.download_bps=999.0;
 for(const auto& [name,watts,capacity]:std::array<std::tuple<const char*,double,int>,4>{
     {{"STATUS_ZERO",0.,0},{"STATUS_CHARGING",2.5,100},{"STATUS_DISCHARGING",-3.5,15},
      {"STATUS_UNAVAILABLE",std::numeric_limits<double>::quiet_NaN(),-1}}}) {
  app.backend.telemetry.net_watts=watts;app.backend.telemetry.capacity=capacity;render(name);
 }
 app.backend.telemetry.upload_bps=999.49;app.backend.telemetry.download_bps=999.5;
 render("STATUS_UNIT_ROUNDING");
 for(const auto* locale:{"zh-CN","en"}) {
  app.backend.preferences.locale=locale;
  app.backend.telemetry.capacity=82;app.backend.telemetry.battery_status="Charging";
  app.backend.telemetry.net_watts=4.92;
  app.backend.telemetry.ncm_state="connected";
  app.backend.telemetry.upload_bps=8950;app.backend.telemetry.download_bps=7400;
  app.backend.telemetry.time_valid=true;app.backend.telemetry.epoch=0;
  app.backend.telemetry.clock="09:11:54";app.backend.telemetry.date="2026-11-25";
  app.backend.preferences.hour=12;
  app.page="SETTINGS";app.focus=1;
  for(const auto* section:{"home","status","time","timeout","traffic","language","keys","save"}){
   app.argument=section;app.choice.clear();render("SETTINGS_"+std::string(section)+"_"+locale);
   if(app.argument=="time"){
    app.focus=3;app.key(KEY_POWER);
    render("TIME_MENU_"+std::string(locale));
    app.back();app.focus=1;
   }
  }
 }
 app.backend.preferences.locale="zh-CN";
 app.page="SETTINGS";app.argument="time";app.focus=3;
 app.depth=Depth{};app.compose();app.key(KEY_POWER);app.compose();
 painter.scene(image.bytes.data(),image.pitch,app);
 png(output/"TIME_MENU_STEREO.png",image,0,canvas_width);
 std::ofstream(output/"alignment.json")<<json_object_to_json_string_ext(report,JSON_C_TO_STRING_PRETTY)<<"\n";
 json_object_put(report);
 std::ofstream(output/"验证说明.txt")<<"离线布局验证：使用原生绘制器与合成状态；"
  "采用无视差绘制以核对字体像素边界。额外任务状态为离线布局样例，未写入实际任务文件。"
  "图像未提交至 KMS，不属于实机扫描输出截图。\n";
 std::cout<<"RENDER_GALLERY_OK\n";return 0;
}
}
int main(int argc,char** argv) {
 try {
  if(argc==3&&std::string_view(argv[1])=="--optics-selftest"){
   const auto valid=parse_optics("0123456789abcdef 1600 48 1080 1080 120\n",eye_size);
   require(valid&&valid->diameter==1600,"测量圆命令解析");
   for(const auto* invalid:{"0123456789abcdef 2162 48 1080 1080 120",
           "0123456789abcdef 1600 48 799 1080 120","0123456789abcdef 80 48 1080 1080 120",
           "0123456789abcdef 1600 48 1080 1080 120 ","0123456789abcdef 1600 48 1080 1080 2147483648",
           "0123456789abcdef 1600 48 1080 1080 0","0123456789abcdex 1600 48 1080 1080 120"})
    require(!parse_optics(invalid,eye_size),"拒绝无效测量圆命令");
   Snapshot shot;shot.pitch=canvas_width*4;shot.bytes.resize(shot.pitch*eye_size);
   draw_optics(shot.bytes.data(),shot.pitch,*valid);
   png(argv[2],shot,0,canvas_width);
   std::cout<<"OPTICS_SELFTEST_OK\n";return 0;
  }
  if(argc==2&&std::string(argv[1])=="--screenshot-delay-test")return screenshot_client(250);
  if(std::string(fs::path(argv[0]).filename()).find("shot")!=std::string::npos || (argc>1&&std::string(argv[1])=="--screenshot"))return screenshot_client();
  if(argc==2&&std::string_view(argv[1])=="--timeout-preferences") {
   const auto p=load_preferences();
   if(!p.durable)return 1;
   std::cout<<p.connect<<" "<<p.idle<<"\n";return 0;
  }
  if(argc==3&&(std::string_view(argv[1])=="--preferences-store"||std::string_view(argv[1])=="--settings-store")) {
   Preferences result,wanted;
   bool ok=false;
   if(std::string_view(argv[1])=="--preferences-store")ok=save_preferences_native(argv[2],result);
   else if(decode_settings(argv[2],wanted))ok=save_settings_native(wanted,result);
   else result.error="invalid settings request";
   auto* j=json_object_new_object();
   json_object_object_add(j,"version",json_object_new_int(result.version));
   json_object_object_add(j,"locale",json_object_new_string(result.locale.c_str()));
   json_object_object_add(j,"storage",json_object_new_string(result.storage.c_str()));
   json_object_object_add(j,"error",json_object_new_string(result.error.c_str()));
   json_object_object_add(j,"language_selected",json_object_new_boolean(result.language_selected));
   json_object_object_add(j,"durable",json_object_new_boolean(result.durable));
   const auto packed=encode_settings(result);
   json_object_object_add(j,"settings",json_object_new_string(packed.c_str()));
   std::cout<<json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN)<<"\n";json_object_put(j);return ok?0:1;
  }
  if(argc>1&&std::string(argv[1])=="--input-selftest") {
   Input input;int selected=0;input_event e{};e.type=EV_KEY;e.code=KEY_POWER;
   e.value=0;input.event(e,[&](int){selected++;});e.value=1;input.event(e,[&](int){selected++;});
   e.value=2;input.event(e,[&](int){selected++;});e.value=0;input.event(e,[&](int){selected++;});
   require(selected==1,"电源松开去重检查");
   e.value=1;input.event(e,[&](int){selected++;});input.began[KEY_POWER]-=1;e.value=0;input.event(e,[&](int){selected++;});require(selected==1,"长按不选择检查");
   e.value=1;input.event(e,[&](int){selected++;});e.type=EV_SYN;e.code=SYN_DROPPED;input.event(e,[&](int){selected++;});e.type=EV_KEY;e.code=KEY_POWER;e.value=0;input.event(e,[&](int){selected++;});require(selected==1,"丢失事件抑制检查");
   int held_edges=0;
   const auto dispatch=[&](int){++selected;};
   const auto reader=[&](int code,int value,double){require(code==KEY_VOLUMEDOWN,"滚动键检查");held_edges+=value==1?1:10;return true;};
   e.type=EV_SYN;e.code=SYN_REPORT;input.event(e,dispatch);
   e.type=EV_KEY;e.code=KEY_VOLUMEDOWN;e.value=1;input.event(e,dispatch,{},reader,{},true);
   input.event(e,dispatch,{},reader,{},true);
   input.began[e.code]-=1;input.repeated[e.code]-=1;e.value=2;
   input.event(e,dispatch,{},reader,{},true);
   input.event(e,dispatch,{},reader,{},false);
   e.value=0;input.event(e,dispatch,{},reader,{},false);
   require(held_edges==11&&selected==1,"阅读器仅响应按下和松开，离开后不续发");
   e.value=1;input.event(e,dispatch,{},reader);
   input.began[e.code]-=1;input.repeated[e.code]-=1;e.value=2;input.event(e,dispatch);
   require(selected==3,"菜单保留系统连发");
   std::cout<<"INPUT_SELFTEST_OK\n";return 0;
  }
  if(argc>1&&std::string(argv[1])=="--input-inventory") {
   std::vector<std::unique_ptr<Input>> inputs;discover(inputs,true);return inputs.size()==3?0:1;
  }
  if(argc==3&&std::string(argv[1])=="--render-gallery")return render_gallery(argv[2]);
  return run(preview_requested());
 }catch(const std::exception& e){std::cerr<<"Recovery 界面："<<e.what()<<"\n";return 1;}
}
