// SPDX-License-Identifier: Unlicense
#include "install_log_locale.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc,char** argv) {
    using p4ui::install_log::message;
    using p4ui::install_log::line;
    if(message("installing gtk3...",false)!="正在安装 gtk3…" ||
       message("Device readback verified super by SHA-256",false).find("super")==std::string::npos ||
       message("warning: permissions differ on /base/etc/",false)!="警告：permissions differ on /base/etc/" ||
       line(">> [  75s] writing archive and manifest",false)!=">> [  75s] 生成用户空间归档和清单" ||
       line("installing gtk3...",true)!="installing gtk3..." ||
       message("unknown external diagnostic: E42 /dev/sda16",false)!="unknown external diagnostic: E42 /dev/sda16")
        return 1;
    for(int i=1;i<argc;++i){
        std::ifstream input(argv[i]);
        if(!input)return 2;
        unsigned total=0, translated=0;
        for(std::string raw;std::getline(input,raw);){
            if(raw.find_first_not_of(" \r\t")==raw.npos)continue;
            std::string_view value(raw);
            if(value.starts_with('[')){
                const auto at=value.find("] ");
                if(at!=value.npos)value.remove_prefix(at+2);
            }
            ++total;
            translated+=line(value,false)!=value;
        }
        std::cout<<"nonempty="<<total<<" translated="<<translated<<'\n';
    }
    std::cout<<"INSTALL_LOG_LOCALE_OK\n";
}
