// SPDX-License-Identifier: Unlicense
#pragma once
#include "ui.hpp"
#include <array>
#include <charconv>

namespace p4ui {
inline std::string encode_settings(const Preferences& p) {
    std::string s=p.locale;
    for (int v : {static_cast<int>(p.visible),p.hour,p.date_style,p.zone,p.units,p.connect,p.idle,
                  int(p.date_first),int(p.reverse)}) s+=","+std::to_string(v);
    return s;
}
inline bool decode_settings(std::string_view s,Preferences& p) {
    if (s.size()>128) return false;
    const auto comma=s.find(',');
    if (comma==s.npos) return false;
    const auto locale=s.substr(0,comma);
    if (locale!="zh-CN" && locale!="en") return false;
    s.remove_prefix(comma+1);
    std::array<int,9> fields{};
    for (std::size_t i=0;i<fields.size();++i) {
        const auto end=s.find(',');
        if ((end==s.npos)!=(i+1==fields.size())) return false;
        const auto part=s.substr(0,end);
        if (part.empty()) return false;
        auto [last,error]=std::from_chars(part.data(),part.data()+part.size(),fields[i]);
        if (error!=std::errc{} || last!=part.data()+part.size()) return false;
        if (end!=s.npos) s.remove_prefix(end+1);
    }
    const auto [visible,hour,date,zone,units,connect,idle,first,reverse]=fields;
    const auto timeout=[](int n){return n==-1 || n==0 || (n>=60 && n<=14400 && n%60==0);};
    if (visible<0 || visible>511 || (hour!=12 && hour!=24) || date<0 || date>3 || zone<0 || zone>5 ||
        units<0 || units>2 || !timeout(connect) || !timeout(idle) ||
        first<0 || first>1 || reverse<0 || reverse>1) return false;
    p.locale=locale;p.visible=visible;p.hour=hour;p.date_style=date;p.zone=zone;p.units=units;
    p.connect=connect;p.idle=idle;p.date_first=first;p.reverse=reverse;
    return true;
}
}
