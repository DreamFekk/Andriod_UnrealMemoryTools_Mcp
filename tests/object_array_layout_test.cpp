#include "UE/ObjectArrayLayout.hpp"
#include <iostream>
#include <stdexcept>

int main(){try {
    int checks=0;
    auto require=[&](bool pass,const char* name){++checks;if(!pass)throw std::runtime_error(name);};
    const uintptr_t base=0x100000,slot=base+0x100,table=base+0x1000,chunk=base+0x2000,objs=base+0x5000,vtable=base+0x4000;
    std::vector<uint8_t> memory(0x300000);
    const auto valid=[&](uintptr_t at){return at>=base && at<base+memory.size();};
    UERuntime::Reader read=[&](uintptr_t at,void* out,size_t n){if(!valid(at)||n>base+memory.size()-at)return false;std::memcpy(out,memory.data()+at-base,n);return true;};
    const auto put=[&]<class T>(uintptr_t at,T v){std::memcpy(memory.data()+at-base,&v,sizeof(v));};
    auto fill=[&](uintptr_t data,uint32_t stride,uint32_t member,uint32_t first,uint32_t count){
        for(uint32_t i=0;i<count;++i){auto obj=objs+uintptr_t(i)*0x100;put(data+uintptr_t(i)*stride+member,obj);put(obj,vtable);put(obj+12,first+i);}
    };
    put(slot,table);put(slot+8,int32_t(64));put(slot+12,int32_t(64));fill(table,24,0,0,64);
    auto layout=UERuntime::ValidateObjectArray(read,slot,valid);
    require(layout && layout.count==64 && layout.itemSize==24 && !layout.chunkSize && layout.indexOffset==12,"flat table validates");
    // Empty slot 1 must not invalidate an otherwise correct table.
    put(table+24,uintptr_t(0));layout=UERuntime::ValidateObjectArray(read,slot,valid);
    require(bool(layout),"hole in first page");
    // A UObject arena can contain real objects/names and arbitrary pointers, but
    // it cannot make their InternalIndex values equal the supposed item slots.
    for(uint32_t i=0;i<64;++i)put(objs+i*0x100+12,i+1000);
    require(!UERuntime::ValidateObjectArray(read,slot,valid),"reject arena with misleading readable objects");
    fill(table,24,0,0,64);put(objs+31*0x100+12,uint32_t(9876));
    require(!UERuntime::ValidateObjectArray(read,slot,valid),"distant sample rejects wrong table");
    std::fill(memory.begin(),memory.end(),0);
    put(slot,table);put(slot+8,int32_t(64));put(slot+12,int32_t(64));fill(table,32,8,0,64);
    layout=UERuntime::ValidateObjectArray(read,slot,valid);
    require(layout && layout.itemSize==32 && layout.itemObject==8,"nonstandard object item stride/member");
    std::fill(memory.begin(),memory.end(),0);
    put(slot,table);put(table,chunk);put(slot+16,int32_t(131072));put(slot+20,int32_t(65568));put(slot+24,int32_t(2));put(slot+28,int32_t(2));
    fill(chunk,24,0,0,32);
    const auto second=base+0x190000;put(table+8,second);
    // Sparse later samples; a second chunk must be dereferenced separately.
    put(second,objs+0x20000);put(objs+0x20000,vtable);put(objs+0x2000c,uint32_t(65536));
    put(second+31*24,objs+0x20100);put(objs+0x20100,vtable);put(objs+0x2010c,uint32_t(65567));
    layout=UERuntime::ValidateObjectArray(read,slot,valid);
    require(layout && layout.chunkSize==65536 && layout.countOffset==20,"chunked table with boundary samples");
    put(slot+28,int32_t(1));require(!UERuntime::ValidateObjectArray(read,slot,valid),"chunk count cannot under-cover elements");
    put(slot+28,int32_t(2));put(slot+20,int32_t(50000000));require(!UERuntime::ValidateObjectArray(read,slot,valid),"reject huge count");
    std::fill(memory.begin(),memory.end(),0);
    put(slot,table);put(slot+8,int32_t(64));put(slot+12,int32_t(64));fill(table,24,0,0,64);
    for(uint32_t i=0;i<64;++i)put(objs+i*0x100+12,i^uint32_t(0x10203040));
    layout=UERuntime::ValidateObjectArray(read,slot,valid,[&](uintptr_t at){return int32_t(UERuntime::Read<uint32_t>(read,at)^0x10203040);});
    require(bool(layout),"profile index decoder used during discovery");
    std::cout<<"PASS: "<<checks<<" object-array layout regression cases\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
