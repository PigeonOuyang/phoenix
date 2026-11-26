// SPDX-License-Identifier: Unlicense
#pragma once
#include "ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace p4ui::progress_ui {
// Layout coordinates are physical pixels in one 2160 px eye.
inline constexpr double scale=eye_size/2160.;
inline constexpr double center=eye_size/2., main_width=1320*scale, main_left=center-main_width/2;
inline constexpr double deck_top=1608*scale, main_axis=1668*scale, main_gap=12*scale;
inline constexpr double expanded_width=456*scale, child_top=1720*scale, first_child_axis=1766*scale;
inline constexpr double child_pitch=52*scale, child_gap=18*scale;
inline constexpr double meter_width=164*scale, status_width=116*scale, min_status_width=108*scale;
inline constexpr std::size_t anchor_steps=5;

struct Segment {
    double x=0,w=0;
    bool operator==(const Segment&) const = default;
};

inline std::vector<Segment> main_rail(std::size_t count,std::size_t active) {
    std::vector<Segment> rail;
    if(!count)return rail;
    const bool expanded=active<count&&count>1;
    const double available=main_width-(count-1)*main_gap;
    const double focus=expanded?std::min(expanded_width,available-(count-1)*24*scale):0;
    const double peer=(available-focus)/(count-expanded);
    double x=main_left;
    for(std::size_t i=0;i<count;++i) {
        const double w=expanded&&i==active?focus:peer;
        rail.push_back({x,w});
        x+=w+main_gap;
    }
    return rail;
}

inline double chord(double y,double inset=0) {
    const double radius=std::hypot(main_width/2,main_axis-center)-inset;
    return 2*std::sqrt(std::max(0.,radius*radius-(y-center)*(y-center)));
}

struct ChildRow {
    std::size_t begin=0,count=0;
    double y=0,w=0,status=status_width;
};
struct ChildLayout {
    double x=center;
    std::vector<ChildRow> rows;
};

inline ChildLayout child_layout(std::span<const unsigned char> measured,double parent) {
    ChildLayout result;
    if(measured.empty())return result;
    constexpr double cap_inset=2.5*scale;
    if(measured.size()<=anchor_steps) {
        double w=(measured.size()-1)*child_gap;
        for(bool meter:measured)w+=meter?meter_width:status_width;
        const double travel=std::max(0.,(chord(first_child_axis,cap_inset)-w)/2);
        result.x=std::clamp(parent,center-travel,center+travel);
        result.rows.push_back({0,measured.size(),first_child_axis,w,status_width});
        return result;
    }

    struct Fit {
        std::size_t shown=0,rows=0;
        double compression=0;
        ChildRow first;
    };
    std::vector<double> axes;
    for(double y=first_child_axis;chord(y,cap_inset)>=min_status_width;y+=child_pitch)
        axes.push_back(y);
    const auto count=measured.size();
    std::vector fits(axes.size()+1,std::vector<Fit>(count+1));
    for(std::size_t row=axes.size();row-- > 0;) {
        const double y=axes[row],room=chord(y,cap_inset);
        for(std::size_t start=count;start-- > 0;) {
            auto& best=fits[row][start];
            std::size_t meters=0;
            for(std::size_t length=1;length<=count-start;++length) {
                meters+=measured[start+length-1]!=0;
                const auto status=length-meters;
                const double base=meters*meter_width+(length-1)*child_gap;
                if(base+status*min_status_width>room)break;
                const double width=status?std::min(status_width,(room-base)/status):status_width;
                const auto& tail=fits[row+1][start+length];
                const Fit next{length+tail.shown,1+tail.rows,
                    status*(status_width-width)+tail.compression,{start,length,y,base+status*width,width}};
                // Prefer fewer rows, less compression, then fuller upper rows.
                if(next.shown>best.shown||(next.shown==best.shown&&
                   (next.rows<best.rows||(next.rows==best.rows&&
                   (next.compression<best.compression-1e-6||
                    (std::abs(next.compression-best.compression)<1e-6&&length>best.first.count))))))
                    best=next;
            }
        }
    }
    for(std::size_t row=0,start=0;row<axes.size()&&fits[row][start].shown;++row) {
        const auto first=fits[row][start].first;
        result.rows.push_back(first);
        start+=first.count;
    }
    return result;
}

template<std::size_t N>
inline double curve(double t,const std::array<std::pair<double,double>,N>& points) {
    for(std::size_t i=1;i<N;++i) {
        const auto [end,to]=points[i];
        if(t<=end) {
            const auto [start,from]=points[i-1];
            return std::lerp(from,to,std::clamp((t-start)/(end-start),0.,1.));
        }
    }
    return 1;
}

inline constexpr std::array<std::pair<double,double>,7> rail_curve{{
    {0,0},{.10,.15},{.25,.53},{.46,.86},{.70,1.02},{.89,.997},{1,1}}};
inline constexpr std::array<std::pair<double,double>,6> deck_open_curve{{
    {0,0},{.12,.29},{.33,.79},{.59,1.08},{.80,.98},{1,1}}};
inline constexpr std::array<std::pair<double,double>,6> deck_close_curve{{
    {0,0},{.16,-.09},{.40,.22},{.74,.81},{.89,1.04},{1,1}}};

struct Fill {
    double from=0,target=0,at=0;
    bool moving(double now) const {return from<target&&now-at<.260;}
    double shown(double now) const {
        const double left=1-std::clamp((now-at)/.260,0.,1.);
        return std::lerp(from,target,1-left*left*left);
    }
    double update(double value,double now,bool animate,bool monotonic=false) {
        const double prior=shown(now);
        if(monotonic)value=std::max(value,prior);
        if(!animate||value<prior)from=target=value;
        else if(value!=target){from=prior;target=value;at=now;}
        return shown(now);
    }
};

struct Animation {
    std::string task,deck;
    std::vector<std::string> stage_ids,child_ids;
    std::vector<Segment> from,to;
    std::vector<Fill> stage_fills,child_fills;
    std::vector<Node> deck_nodes;
    double rail_at=0,deck_at=0;
    bool closing=false,tween=false,activity=false;

    bool rail_moving(double now) const {return from!=to&&now-rail_at<.300;}
    std::vector<Segment> rail(std::vector<Segment> target,double now,bool animate) {
        const auto sample=[&] {
            if(!rail_moving(now))return to;
            auto shown=to;
            const double t=curve((now-rail_at)/.300,rail_curve);
            for(std::size_t i=0;i<shown.size();++i)
                shown[i]={std::lerp(from[i].x,to[i].x,t),std::lerp(from[i].w,to[i].w,t)};
            return shown;
        };
        if(!animate||to.size()!=target.size())from=to=std::move(target);
        else if(to!=target){from=sample();to=std::move(target);rail_at=now;}
        return sample();
    }
};

using Measure = std::function<double(std::string_view,double,bool)>;
bool has_log_deck(const App& app);
void draw_log_deck(const App& app,Animation& motion,const Measure& measure,std::vector<Node>& out);
}
