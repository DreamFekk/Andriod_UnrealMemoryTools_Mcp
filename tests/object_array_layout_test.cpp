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

    // Real Android pointers have a plausible small count in their high DWORD.
    // Reproduce the Delta Force header order with a separate high-address image.
    if constexpr(sizeof(uintptr_t)==8) {
        const uintptr_t highBase=0x6e4cc00000ULL,highSlot=highBase+0x100,
            highTable=highBase+0x1000,highObjects=highBase+0x10000,highVtable=highBase+0x4000;
        std::vector<uint8_t> highMemory(0x50000);
        const auto highValid=[&](uintptr_t at){return at>=highBase && at-highBase<highMemory.size();};
        UERuntime::Reader highRead=[&](uintptr_t at,void* out,size_t n){
            if(!highValid(at)||n>highMemory.size()-(at-highBase))return false;
            std::memcpy(out,highMemory.data()+(at-highBase),n);return true;
        };
        const auto highPut=[&]<class T>(uintptr_t at,T value){std::memcpy(highMemory.data()+(at-highBase),&value,sizeof(value));};
        constexpr uint32_t actualCount=384,actualCapacity=512;
        highPut(highSlot,highTable);
        for(uint32_t i=0;i<actualCount;++i) {
            const auto object=highObjects+uintptr_t(i)*0x100;
            highPut(highTable+uintptr_t(i)*32+8,object);
            highPut(object,highVtable);highPut(object+0x24,i);
        }
        require(UERuntime::Read<uint32_t>(highRead,highSlot+4)==110,"fixture reproduces pointer-high false count");
        require(!UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,4,0x24),
                "pointer high DWORD alone cannot validate an object count");
        highPut(highSlot+8,actualCount);highPut(highSlot+12,actualCapacity);
        auto highLayout=UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,4,0x24);
        require(highLayout && highLayout.count==actualCount && highLayout.capacity==actualCapacity &&
                highLayout.countOffset==8 && highLayout.itemSize==32 && highLayout.itemObject==8 &&
                highLayout.indexOffset==0x24 && !highLayout.chunkSize,
                "reordered count then capacity header preserves complete high-address array");
        require(UERuntime::ArrayObject(highRead,highLayout,actualCount-1)==highObjects+(actualCount-1)*0x100,
                "validated final object remains reachable beyond pointer-high prefix");
        highPut(highObjects+uintptr_t(actualCount-1)*0x100+0x24,actualCount+1000);
        require(!UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,8,0x24),
                "compacted subset with matching prefix but mismatched final index is rejected");
        highPut(highObjects+uintptr_t(actualCount-1)*0x100+0x24,actualCount-1);
        highPut(highSlot+8,uint32_t(110));highPut(highSlot+12,uint32_t(110));
        require(!UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,8,0x24),
                "live indexed object after count rejects truncated header aliases");
        highPut(highSlot+8,actualCapacity);highPut(highSlot+12,actualCapacity);
        highLayout=UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,8,0x24);
        require(highLayout && highLayout.count==actualCapacity,
                "valid count-capacity header permits a long empty high-water tail after GC");
        highPut(highSlot+8,uint32_t(0));highPut(highSlot+12,uint32_t(0));
        highPut(highSlot-16,actualCapacity);highPut(highSlot-12,actualCount);
        highLayout=UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,-12,0x24);
        require(highLayout && highLayout.countOffset==-12 && highLayout.count==actualCount &&
                highLayout.capacity==actualCapacity,"signed count offset supports metadata before Objects pointer");
        highPut(highTable+uintptr_t(actualCount-1)*32+8,uintptr_t(0));
        highLayout=UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,-12,0x24);
        require(bool(highLayout),"bounded tail validation permits a trailing object hole");
        for(uint32_t i=actualCount-64;i<actualCount;++i)highPut(highTable+uintptr_t(i)*32+8,uintptr_t(0));
        highLayout=UERuntime::ValidateObjectArray(highRead,highSlot,highValid,{},32,-12,0x24);
        require(highLayout && highLayout.count==actualCount,"signed header preserves more than 32 trailing GC holes");
        require(UERuntime::ObjectArrayFieldAddress(8,-12)==0 &&
                UERuntime::ObjectArrayFieldAddress(std::numeric_limits<uintptr_t>::max()-1,8)==0,
                "relative metadata address arithmetic rejects underflow and overflow");
    }
    std::cout<<"PASS: "<<checks<<" object-array layout regression cases\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
