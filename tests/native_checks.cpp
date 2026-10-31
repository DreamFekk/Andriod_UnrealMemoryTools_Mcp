#include "FastDump.hpp"
#include <KittyScanner.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
struct Memory final : IKittyMemOp {
    std::vector<char> bytes;
    bool shortRead=false;
    bool init(pid_t) override { return true; }
    size_t Read(uintptr_t address,void* result,size_t length) const override {
        if(address<0x10000 || address-0x10000>=bytes.size()) return 0;
        auto n=std::min(length,bytes.size()-(address-0x10000));
        if(shortRead && n) --n;
        memcpy(result,bytes.data()+address-0x10000,n); return n;
    }
    size_t Write(uintptr_t,void*,size_t) const override {return 0;}
};
int main() {
    try {
        Memory mem;
        KittyScannerMgr scanner(&mem);
        std::mt19937 random(62819);
        for(unsigned round=0;round<5000;++round) {
            mem.bytes.resize(1+random()%512);
            for(auto& c:mem.bytes)c=static_cast<char>(random()%16);
            std::string pattern(1+random()%40,'\0'),mask(pattern.size(),'?');
            for(size_t i=0;i<pattern.size();++i) {pattern[i]=static_cast<char>(random()%16);mask[i]=random()%3?'x':'?';}
            if(round%2 && pattern.size()<=mem.bytes.size()) {
                auto pos=random()%(mem.bytes.size()-pattern.size()+1);
                for(size_t i=0;i<pattern.size();++i) if(mask[i]=='x')mem.bytes[pos+i]=pattern[i];
            }
            uintptr_t expected=0;
            for(size_t pos=0;pos+pattern.size()<=mem.bytes.size();++pos) {
                bool match=true;
                for(size_t i=0;i<pattern.size();++i) if(mask[i]=='x' && mem.bytes[pos+i]!=pattern[i]) {match=false;break;}
                if(match) {expected=0x10000+pos;break;}
            }
            Require(scanner.findBytesFirst(0x10000,0x10000+mem.bytes.size(),pattern.data(),mask)==expected,"pattern mismatch");
        }
        mem.bytes.assign(128,'A'); mem.shortRead=true;
        Require(scanner.findBytesFirst(0x10000,0x10080,"\0","x")==0,"short-read tail matched");
        const std::string path="/data/local/tmp/umt-native-check-"+std::to_string(getpid())+".bin";
        std::vector<char> source(2*1024*1024+137);
        for(size_t i=0;i<source.size();++i) source[i]=static_cast<char>(i*31);
        // Android's allocator may set a top-byte tag; procfs takes untagged VAs.
        const uintptr_t address = reinterpret_cast<uintptr_t>(source.data()) & 0x00ffffffffffffffULL;
        auto result=FastDump::Range(getpid(),address,address+source.size(),path,{},2);
        if(!result.ok) fprintf(stderr,"dump error: %s\n",result.error.c_str());
        Require(result.ok,"dump self memory failed");
        std::ifstream in(path,std::ios::binary);
        std::vector<char> copy((std::istreambuf_iterator<char>(in)),{});
        Require(copy==source,"dump did not preserve exact tail bytes/length");
        const auto bad=FastDump::Range(getpid(),1,4097,path,{},1);
        Require(!bad.ok,"invalid memory dump reported success");
        std::ifstream preserved(path,std::ios::binary);
        std::vector<char> old((std::istreambuf_iterator<char>(preserved)),{});
        Require(old==source,"failed dump overwrote previous output");
        unlink(path.c_str()); unlink((path+".partial").c_str());
        puts("PASS: 5000 randomized scanner cases, short read, exact dump length/content, failed-dump preservation");
        return 0;
    } catch(const std::exception& error) {fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
