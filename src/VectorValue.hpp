#pragma once
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace VectorValue {
inline int Width(std::string_view name, size_t bytes) {
    if(name=="Vector3f") return bytes==12?4:0;
    if(name=="Vector3d") return bytes==24?8:0;
    if(name=="Vector") return bytes==12?4:bytes==24?8:0;
    if(name=="Vector_NetQuantize" || name=="Vector_NetQuantize10" ||
       name=="Vector_NetQuantize100" || name=="Vector_NetQuantizeNormal")
        return bytes==12?4:bytes==24?8:0;
    return 0;
}
inline std::array<double,3> Decode(const void* bytes,int width) {
    std::array<double,3> result{};
    for(size_t i=0;i<3;++i) {
        if(width==4) {float value;std::memcpy(&value,static_cast<const char*>(bytes)+i*4,4);result[i]=value;}
        else if(width==8) std::memcpy(&result[i],static_cast<const char*>(bytes)+i*8,8);
    }
    return result;
}
inline std::string Format(const std::array<double,3>& v,int width) {
    char text[256];
    const int precision=width==8?17:9;
    std::snprintf(text,sizeof(text),"X=%.*g  Y=%.*g  Z=%.*g",precision,v[0],precision,v[1],precision,v[2]);
    return text;
}
}
