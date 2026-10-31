// Portable reader regression: g++ -std=c++20 -O2 -Isrc tests/runtime_codec_test.cpp -o runtime_codec_test
#include "UE/UERuntimeSupport.hpp"
#include <iostream>
#include <stdexcept>
#include <tuple>

int main(){try {
    int checks=0;
    const auto check=[&](bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);};
    std::vector<uint8_t> bytes(0x80000);
    const uintptr_t base=0x100000;
    UERuntime::Reader read=[&](uintptr_t at,void* out,size_t n){
        if(at<base || at-base>bytes.size() || n>bytes.size()-(at-base))return false;
        std::memcpy(out,bytes.data()+at-base,n);return true;};
    auto put=[&]<class T>(uintptr_t at,T value){std::memcpy(bytes.data()+at-base,&value,sizeof(value));};
    auto raw=[&](uintptr_t at,const void* data,size_t n){std::memcpy(bytes.data()+at-base,data,n);};
    UE_Offsets o;
    o.Config.IsUsingFNamePool=true;o.Config.isUsingOutlineNumberName=true;
    o.FNamePool={2,16,0x40};o.FNamePoolEntry.Header=0;
    o.FNamePoolEntry.GetLength=[](uint16_t h)->size_t{return h>>6;};
    o.FNamePoolEntry.GetIsWide=[](uint16_t h){return (h&1)!=0;};
    const uintptr_t pool=base+0x100,block=base+0x1000;
    put(pool+0x40,block);
    auto resolve=[&](uint32_t id){return UERuntime::PoolEntry(read,pool,id,o);};
    auto name=[&](uintptr_t at){return UERuntime::Name(read,at,o,resolve);};
    put(block,uint16_t(4<<6));raw(block+2,"None",4);
    put(block+6,uint16_t(12<<6));raw(block+8,"ByteProperty",12);
    check(name(resolve(0))=="None","None");check(name(resolve(3))=="ByteProperty","ByteProperty");
    put(block+32,uint16_t(0));put(block+34,uint32_t(0));put(block+38,uint32_t(2));
    check(name(block+32)=="None_1","outline zero id");
    put(block+34,uint32_t(16));check(name(block+32).empty(),"outline cycle");
    o.Config.isUsingOutlineNumberName=false;check(name(block+32).empty(),"outline disabled");
    put(block+64,uint16_t((4<<6)|1));raw(block+66,u"对象\U0001F600",8);
    check(name(block+64)=="对象\xF0\x9F\x98\x80","UTF16 surrogate");
    put(block+66,char16_t(0xd800));put(block+68,char16_t('A'));check(name(block+64).empty(),"malformed UTF16");
    const UERuntime::Reader shortRead=[](uintptr_t,void* out,size_t n){std::memset(out,0xff,n);return false;};
    check(UERuntime::Read<uint64_t>(shortRead,base)==0,"failed partial read zeroes result");
    o.FNamePool.BlocksBit=64;check(!resolve(3),"invalid pool shift");o.FNamePool.BlocksBit=16;
    check(!resolve(0x7fff0000),"pool block cap");
    put(base+bytes.size()-2,uint16_t(63<<6));check(name(base+bytes.size()-2).empty(),"short pool payload");
    const auto legacy=block+128;
    o.Config.IsUsingFNamePool=false;o.FNameEntry.Index=8;o.FNameEntry.Name=12;
    put(legacy+8,uint32_t(1));raw(legacy+12,u"测试",6);
    check(UERuntime::Name(read,legacy,o,{})=="测试","legacy UTF16");
    put(legacy+8,uint32_t(0));raw(legacy+12,"NameWithMoreThanSixteenCharacters",33);
    check(UERuntime::Name(read,legacy,o,{})=="NameWithMoreThanSixteenCharacters","batched legacy ASCII");
    for(const auto [input,major,minor]:std::vector<std::tuple<std::string,int,int>>{
        {"++UE5+Release-5.6",5,6},{"++UE4+Release-5.2",5,2},{"abc+Release-4.27xyz",4,27},{"++UE5+Main-5.7",5,7}}) {
        const auto v=UERuntime::ParseVersion(input);check(v.major==major && v.minor==minor,"version parsing");
    }
    check(!UERuntime::ParseVersion("++UE5+Release-5."),"truncated version");
    check(!UERuntime::ParseVersion("5.6"),"unanchored version");
    std::array<uint8_t,16> enc{0x3b,0xcf,0xcd,0xca,0x84,0x1d,0x32,0x34,0x7f,0x39,0x53,0xd5,0xc0,0xe4,0xe7,0x22};
    UERuntime::NewStateNames(enc.data(),enc.size());
    check(std::string((char*)enc.data(),16)=="NoneByteProperty","New State golden ANSI");
    char16_t wide[]{0x4e3b,0x65cf,0x67cd,0x8bca};UERuntime::NewStateNames(wide,4);
    check(wide[0]==0x4e4e && wide[3]==0x8b65,"New State UTF16 high byte");
    check(UERuntime::NewStatePointer({0,1,1})==0,"New State null");
    const auto descriptor=base+0x3000;
    for(int32_t code:{INT32_MIN,0,100,102,151,INT32_MAX}) {put(descriptor,code);check(!UERuntime::PubgChain(read,descriptor),"PUBG length bound");}
    put(descriptor,int32_t(103));put(descriptor+8,block);check(UERuntime::PubgChain(read,descriptor)==block,"PUBG one hop");
    put(descriptor,int32_t(106));put(descriptor+8,descriptor+64);put(descriptor+64,descriptor+64);check(!UERuntime::PubgChain(read,descriptor),"PUBG cycle");
    const auto header=base+0x4000,names=base+0x5000,values=base+0x6000;
    const auto enumName=[](uintptr_t at){return "Enum::Value"+std::to_string(at);};
    put(header,names);put(header+8,int32_t(3));put(header+12,int32_t(3));
    put(names+8,int64_t(-1));put(names+24,int64_t(42));put(names+40,int64_t(42));
    auto e=UERuntime::EnumValues(read,header,8,enumName);
    check(e.size()==3 && e[0].second==-1 && e[2].second==42,"legacy sparse signed alias enum");
    put(header,names|1);put(header+8,values|1);put(header+16,int32_t(3));
    put(values,int64_t(-1));put(values+8,int64_t(42));put(values+16,int64_t(42));
    e=UERuntime::EnumValues(read,header,4,enumName);
    check(e.size()==3 && e[0].second==-1 && e[2].second==42,"UE5 split enum");
    check(UERuntime::EnumValues(read,header,4,enumName,1).size()==1,"enum row cap");
    put(header+16,int32_t(100000));check(UERuntime::EnumValues(read,header,4,enumName).empty(),"enum count validation");
    std::cout<<"PASS: "<<checks<<" portable engine codec checks\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
