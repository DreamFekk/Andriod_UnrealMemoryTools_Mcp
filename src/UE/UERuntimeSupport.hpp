#pragma once

#include "UEOffsets.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <string_view>
#include <vector>

// Bounded, independently testable readers. All addresses belong to the target;
// no target code is executed and no process-global decryption state is retained.
namespace UERuntime {
using Reader = std::function<bool(uintptr_t, void*, size_t)>;
using DecodeAnsi = std::function<void(char*, size_t)>;
using DecodeWide = std::function<void(char16_t*, size_t)>;
template<class T> T Read(const Reader& read, uintptr_t address) {
    T value{}; if (!address || !read(address, &value, sizeof(value))) return T{}; return value;
}
inline bool Pointer(uintptr_t p) { return p >= 0x10000 && p < 0x0001000000000000ULL && !(p & (sizeof(uintptr_t)-1)); }

struct Version { int major=0, minor=0; explicit operator bool() const {return major==4 || major==5;} };
inline Version ParseVersion(std::string_view data) {
    for (auto marker : {"++UE5+Release-", "++UE4+Release-", "++UE5+Main-", "+Release-"}) {
        for (size_t pos=data.find(marker); pos!=data.npos; pos=data.find(marker,pos+1)) {
            const char* start=data.data()+pos+std::char_traits<char>::length(marker);
            const char* end=data.data()+data.size(); Version result;
            auto a=std::from_chars(start,end,result.major);
            if(a.ec!=std::errc{} || a.ptr==end || *a.ptr!='.') continue;
            auto b=std::from_chars(a.ptr+1,end,result.minor);
            if(b.ec==std::errc{} && result && result.minor>=0 && result.minor<=99) return result;
        }
    }
    return {};
}

inline std::string Utf16(const char16_t* input, size_t count) {
    std::string out;
    for(size_t i=0;i<count;++i) {
        uint32_t c=input[i];
        if(c>=0xd800 && c<=0xdbff) {
            if(i+1>=count || input[i+1]<0xdc00 || input[i+1]>0xdfff) return {};
            c=0x10000+((c-0xd800)<<10)+(input[++i]-0xdc00);
        } else if(c>=0xdc00 && c<=0xdfff) return {};
        if(c<0x80) out+=char(c);
        else if(c<0x800) {out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
        else if(c<0x10000) {out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else {out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    return out;
}
inline uintptr_t PoolEntry(const Reader& read, uintptr_t pool, uint32_t id, const UE_Offsets& o) {
    const auto bits=o.FNamePool.BlocksBit, stride=o.FNamePool.Stride;
    if(!pool || bits<1 || bits>20 || !stride || stride>8) return 0;
    const auto block=id>>bits;
    if(block>=8192) return 0;
    const auto chunk=Read<uintptr_t>(read,pool+o.FNamePool.BlocksOff+block*sizeof(uintptr_t));
    if(!Pointer(chunk)) return 0;
    return chunk+(id&((uint32_t(1)<<bits)-1))*stride;
}
inline std::string Name(const Reader& read, uintptr_t entry, const UE_Offsets& o,
                        const std::function<uintptr_t(uint32_t)>& resolve,
                        const DecodeAnsi& ansi={}, const DecodeWide& wide={}) {
    if(!entry) return {};
    size_t length=0; bool isWide=false; uint32_t number=0;
    std::array<uintptr_t,8> seen{}; size_t depth=0;
    if(o.Config.IsUsingFNamePool) {
        if(!o.FNamePoolEntry.GetLength || !o.FNamePoolEntry.GetIsWide) return {};
        for(;;) {
            if(!entry || depth==seen.size() || std::find(seen.begin(),seen.begin()+depth,entry)!=seen.begin()+depth) return {};
            seen[depth++]=entry;
            uint16_t header=0;
            if(!read(entry+o.FNamePoolEntry.Header,&header,sizeof(header))) return {};
            length=o.FNamePoolEntry.GetLength(header); isWide=o.FNamePoolEntry.GetIsWide(header);
            if(length) {entry+=o.FNamePoolEntry.Header+2;break;}
            if(!o.Config.isUsingOutlineNumberName || !resolve) return {};
            // Non-case-preserving FNumberedData is packed at +2; case-preserving at +8.
            const uintptr_t payload=o.FNamePoolEntry.Header+2+(o.FNamePoolEntry.Header==4?2:0);
            uint32_t data[2]{};
            if(!read(entry+payload,data,sizeof(data))) return {};
            if(depth==1) number=data[1];
            entry=resolve(data[0]); // ID zero is a valid reference to "None".
        }
        if(length>1023) return {};
    } else {
        uint32_t index=0;
        if(!read(entry+o.FNameEntry.Index,&index,sizeof(index))) return {};
        isWide=o.FNameEntry.GetIsWide?o.FNameEntry.GetIsWide(index):(index&1)!=0;
        entry+=o.FNameEntry.Name;
        // Batch legacy reads too; one syscall per character makes the live
        // inspector much slower. Clamp batches to page boundaries and fall back
        // to one code unit for short/read-protected tails.
        const size_t width=isWide?2:1;
        bool terminated=false;
        while(length<1023 && !terminated) {
            const auto at=entry+length*width;
            size_t units=std::min({size_t(16),size_t(1023-length),size_t((4096-(at&4095))/width)});
            units=std::max(size_t(1),units);
            std::array<uint8_t,32> data{};
            if(!read(at,data.data(),units*width)) {
                units=1;
                if(!read(at,data.data(),width)) return {};
            }
            for(size_t i=0;i<units;++i) {
                uint16_t ch=0;std::memcpy(&ch,data.data()+i*width,width);
                if(!ch) {terminated=true;break;}
                ++length;
            }
        }
        if(!length || !terminated) return {};
    }
    std::string result;
    if(isWide) {
        std::vector<char16_t> units(length);
        if(!read(entry,units.data(),length*2)) return {};
        if(wide) wide(units.data(),length);
        result=Utf16(units.data(),length);
    } else {
        result.resize(length);
        if(!read(entry,result.data(),length)) return {};
        if(ansi) ansi(result.data(),length);
    }
    if(number && !result.empty()) result+='_'+std::to_string(number-1);
    return result;
}

// New State callbacks ported from MobileDumper-7 Shared/NewState.h (MJx0/droidxrx).
inline uint32_t NewStateIndex(const std::array<uint32_t,3>& v) {
    return v[0] && v[1] && v[2] ? v[0]^std::rotr(v[2]^v[1]^uint32_t(0xB5C76D21),7) : 0;
}
inline uint64_t NewStatePointer(const std::array<uint64_t,3>& v) {
    return v[0] && v[1] && v[2] ? v[0]^std::rotr(v[2]^v[1]^uint64_t(0xD2CA397CEA681802),1) : 0;
}
template<class Unit> void NewStateNames(Unit* data,size_t count) {
    uint64_t key=354233679;
    for(size_t i=0;i<count;++i) {
        const uint8_t c=uint8_t(data[i]);
        const auto decoded=uint8_t((((c>>1)&7)|((c&1)<<3)|(((c>>1)&8)<<4)|((c>>5)<<4))-uint8_t(key));
        data[i]=Unit((uint16_t(data[i])&0xff00)|decoded);
        const auto k=uint32_t(key);
        key+=((((k>>13)|uint8_t(key>>8))&3)|(((k>>22)&0x1c)<<3)|(((k>>27)&0x1f)<<3))^0x91;
        key=std::rotr(key,56);
    }
}
inline uintptr_t PubgChain(const Reader& read,uintptr_t address) {
    int32_t code=0;
    if(!read(address,&code,4) || code<103 || code>150) return 0;
    const int hops=(code-100)/3;
    if(hops<1 || hops>16) return 0;
    uintptr_t value=Read<uintptr_t>(read,address+8);
    std::array<uintptr_t,16> seen{};
    for(int i=0;i<hops;++i) {
        if(!Pointer(value) || std::find(seen.begin(),seen.begin()+i,value)!=seen.begin()+i) return 0;
        seen[i]=value;
        if(i+1<hops && !read(value,&value,sizeof(value))) return 0;
    }
    return value;
}

inline std::vector<std::pair<std::string,int64_t>> EnumValues(
    const Reader& read,uintptr_t header,size_t nameSize,
    const std::function<std::string(uintptr_t)>& name,size_t limit=65536) {
    std::vector<std::pair<std::string,int64_t>> out;
    if(nameSize<4 || nameSize>16 || !name) return out;
    const uintptr_t first=Read<uintptr_t>(read,header);
    uintptr_t names=first,values=0;int32_t count=0;
    const bool split=(first&1)!=0;
    if(split) {
        // UE5.6 dynamic FName storage: tagged names*, values*, count.
        names=first&~uintptr_t(1);
        values=Read<uintptr_t>(read,header+sizeof(uintptr_t))&~uintptr_t(1);
        count=Read<int32_t>(read,header+2*sizeof(uintptr_t));
        if(!Pointer(values)) return out;
    } else {
        count=Read<int32_t>(read,header+sizeof(uintptr_t));
        const auto capacity=Read<int32_t>(read,header+sizeof(uintptr_t)+4);
        if(capacity<count || capacity>65536) return out;
    }
    if(!Pointer(names) || count<0 || count>65536) return out;
    const size_t valueOffset=(nameSize+7)&~size_t(7),stride=split?nameSize:valueOffset+8;
    for(size_t i=0;i<std::min(size_t(count),limit);++i) {
        const auto text=name(names+i*stride);int64_t value=0;
        if(text.empty() || text=="None" || !read(split?values+i*8:names+i*stride+valueOffset,&value,8)) return {};
        out.emplace_back(text,value);
    }
    return out;
}
} // namespace UERuntime
