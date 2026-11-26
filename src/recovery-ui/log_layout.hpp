// SPDX-License-Identifier: Unlicense
#pragma once
#include "ui.hpp"
#include <charconv>

namespace p4ui {
inline int parse_log_width(std::string_view s) {
 if (s.ends_with('\n')) s.remove_suffix(1);
 int width=0;
 if(s.empty()||s.size()>4)return log_width_default;
 const auto [end,err]=std::from_chars(s.data(),s.data()+s.size(),width);
 return err==std::errc{}&&end==s.data()+s.size()&&width>=log_width_min&&
  width<=log_width_max&&width%2==0?width:log_width_default;
}
}
