// SPDX-License-Identifier: Unlicense
#pragma once
#include "ui.hpp"
#include "breeze/breeze_index.hpp"
#include <cairo/cairo.h>
#include <png.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace p4ui {
class BreezeAtlas {
    struct Tile {
        std::array<std::uint32_t, breeze_side*breeze_side> pixels{};
        cairo_surface_t* surface=nullptr;
        ~Tile() { if (surface) cairo_surface_destroy(surface); }
    };
    std::vector<unsigned char> rgba;
    unsigned stride=0;
    std::map<std::tuple<int,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t>,Tile> tiles;
    static std::uint32_t rgb(Color c) {
        const auto chan=[](double v){return std::uint32_t(std::lround(std::clamp(v,0.,1.)*255));};
        return chan(c.r)<<16 | chan(c.g)<<8 | chan(c.b);
    }
    static int index(const std::string& name) {
        std::string key=name;
        if (name.starts_with("ncm:")) {
            const auto state=name.substr(4);
            if (state=="connected") key="usb";
            else if (state=="unknown") key="battery-unknown";
            else if (state=="disconnected") key="usb-off";
            else key="ncm-"+state;
        } else if (name.starts_with("battery:")) {
            const auto sep=name.find(':',8);
            const auto value=name.substr(8,sep-8);
            if (value=="unknown") key="battery-unknown";
            else {
                const int v=std::stoi(value);
                const int step=v==0||v==100?v:std::clamp((v+5)/10,1,9)*10;
                key="battery-"+std::string(step<10?"00":step<100?"0":"")+std::to_string(step);
                if (sep!=std::string::npos && name.substr(sep+1)=="Charging") key+="-charging";
            }
        } else if (name=="bluetooth-off") key="bluetooth-unknown";
        else if (name=="wifi-off") key="wifi-unknown";
        else if (name=="ethernet-off") key="ethernet-unknown";
        for (std::size_t i=0;i<std::size(breeze_entries);++i)
            if (breeze_entries[i].key==key) return static_cast<int>(i);
        throw std::runtime_error("Missing pinned Breeze icon: "+name);
    }
public:
    explicit BreezeAtlas(const std::string& path) {
        png_image file{};file.version=PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_file(&file,path.c_str()))
            throw std::runtime_error("Breeze atlas: "+path+" ("+file.message+")");
        if (file.width!=breeze_side*breeze_cols ||
            file.height!=breeze_side*((std::size(breeze_entries)+breeze_cols-1)/breeze_cols)) {
            png_image_free(&file);throw std::runtime_error("Invalid Breeze atlas size");
        }
        file.format=PNG_FORMAT_RGBA;stride=file.width*4;rgba.resize(stride*file.height);
        if (!png_image_finish_read(&file,nullptr,rgba.data(),0,nullptr)) {
            const std::string error=file.message;png_image_free(&file);
            throw std::runtime_error("Breeze atlas decode: "+error);
        }
        png_image_free(&file);
    }
    std::array<double,4> metrics(const Node& n) const {
        const auto& e=breeze_entries[index(n.value)];
        return {double(e.l),double(e.t),double(e.r-e.l),double(e.b-e.t)};
    }
    void draw(cairo_t* c,const Node& n) {
        const int j=index(n.value);
        const auto cutout=rgb(n.color.r<.25?selection:window);
        const auto fg=rgb(n.color);
        const auto blue=rgb(n.ink_height>0?selection:positive);
        const auto red=rgb(n.ink_height>0&&!n.value.starts_with("battery:")?n.color:negative);
        if (tiles.size()>256) tiles.clear();
        auto [at,fresh]=tiles.try_emplace({j,fg,cutout,red,blue});
        auto& tile=at->second;
        if (fresh) {
            for (int y=0;y<breeze_side;y++) for(int x=0;x<breeze_side;x++) {
                const auto* p=&rgba[(j/breeze_cols*breeze_side+y)*stride+(j%breeze_cols*breeze_side+x)*4];
                const unsigned a=std::min(255u,unsigned(p[0])+p[1]+p[2]+p[3]);
                auto channel=[&](int shift){
                    return std::min(a,((fg>>shift&255)*p[0]+(blue>>shift&255)*p[1]+
                              (red>>shift&255)*p[2]+(cutout>>shift&255)*p[3]+127)/255);
                };
                tile.pixels[y*breeze_side+x]=a<<24|channel(16)<<16|channel(8)<<8|channel(0);
            }
            tile.surface=cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(tile.pixels.data()),
                CAIRO_FORMAT_ARGB32,breeze_side,breeze_side,breeze_side*4);
            if (cairo_surface_status(tile.surface)!=CAIRO_STATUS_SUCCESS) throw std::runtime_error("Breeze icon cache failed");
        }
        cairo_save(c);
        const auto& e=breeze_entries[j];
        double sx=n.w/breeze_side,sy=n.h/breeze_side,dx=n.x,dy=n.y;
        if (n.ink_height>0) {
            sx=sy=std::min(n.ink_height/(e.b-e.t),n.w/(e.r-e.l));
            dx+=(n.w-(e.r-e.l)*sx)/2-e.l*sx;
            dy+=(n.h-(e.b-e.t)*sy)/2-e.t*sy;
        }
        cairo_rectangle(c,n.x,n.y,n.w,n.h);cairo_clip(c);
        cairo_translate(c,dx,dy);cairo_scale(c,sx,sy);
        cairo_set_source_surface(c,tile.surface,0,0);cairo_pattern_set_filter(cairo_get_source(c),CAIRO_FILTER_BILINEAR);
        cairo_paint_with_alpha(c,n.color.a);cairo_restore(c);
    }
};
}
