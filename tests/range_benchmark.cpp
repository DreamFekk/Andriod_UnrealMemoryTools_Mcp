#include "FastDump.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
int main(int argc,char** argv) {
    if(argc!=5) {puts("usage: umt_range_benchmark PID START_HEX END_HEX OUTPUT");return 2;}
    const auto pid=static_cast<pid_t>(strtol(argv[1],nullptr,10));
    const auto start=strtoull(argv[2],nullptr,16),end=strtoull(argv[3],nullptr,16);
    for(unsigned repeat=0;repeat<2;++repeat) for(unsigned workers:{1,2,4}) {
        const auto r=FastDump::Range(pid,start,end,argv[4],{},workers);
        printf("workers=%u repeat=%u ok=%d bytes=%llu calls=%llu ms=%.3f MiB_s=%.2f error=%s\n",workers,repeat,r.ok,
            static_cast<unsigned long long>(r.bytes),static_cast<unsigned long long>(r.readCalls),r.milliseconds,
            r.milliseconds>0?r.bytes/1048.576/r.milliseconds:0,r.error.c_str());
        fflush(stdout);
        if(!r.ok)return 1;
    }
    unlink(argv[4]);
    return 0;
}
