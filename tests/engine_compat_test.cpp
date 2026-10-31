#include "UE/UERuntimeSupport.hpp"
#include "UE/UEGameProfiles/NewState.hpp"
#include "UE/UEWrappers.hpp"
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace {
int checks=0;
void Require(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
struct Memory {
    static constexpr uintptr_t base=0x100000;
    std::vector<uint8_t> bytes=std::vector<uint8_t>(0x80000);
    bool read(uintptr_t at,void* out,size_t n) {
        if(at<base || at-base>bytes.size() || n>bytes.size()-(at-base)) return false;
        std::memcpy(out,bytes.data()+at-base,n);return true;
    }
    void raw(uintptr_t at,const void* data,size_t n){std::memcpy(bytes.data()+at-base,data,n);}
    template<class T> void put(uintptr_t at,T value){raw(at,&value,sizeof(value));}
    UERuntime::Reader reader(){return [this](uintptr_t at,void* out,size_t n){return read(at,out,n);};}
};
struct Probe : IGameProfile {
    mutable UE_Offsets offsets=UE_DefaultOffsets::UE5_03(false,false);
    using IGameProfile::DetectNamePoolBlock;
    using IGameProfile::ResolveLegacyTable;
    bool ArchSupprted()const override{return true;}
    std::string GetAppName()const override{return "fixture";}
    std::vector<std::string> GetAppIDs()const override{return {"fixture"};}
    bool isUsingCasePreservingName()const override{return offsets.Config.isUsingCasePreservingName;}
    bool IsUsingFNamePool()const override{return offsets.Config.IsUsingFNamePool;}
    bool isUsingOutlineNumberName()const override{return offsets.Config.isUsingOutlineNumberName;}
    UE_Offsets* GetOffsets()const override{return &offsets;}
    uintptr_t GetMatrix()const override{return 0;}
    uintptr_t GetPhysx()const override{return 0;}
    uintptr_t GetFrameCount()const override{return 0;}
};
struct Vars : UEVars {
    Vars(UE_Offsets& offsets,const NewStateProfile* profile=nullptr) {
        Offsets=&offsets;
        pGetNameByID=[](int32_t id)->std::string {
            switch(id) {case 1:return "IndexName";case 2:return "DisplayName";case 3:return "DoubleProperty";
                case 4:return "OptionalProperty";case 5:return "LargeWorldCoordinatesRealProperty";
                case 6:return "ObjectPtrProperty";default:return "None";}
        };
        if(profile) {
            pObjectIndex=[profile](uintptr_t at){return profile->ReadObjectIndex(at);};
            pObjectClass=[profile](uintptr_t at){return profile->ReadObjectClass(at);};
            pObjectOuter=[profile](uintptr_t at){return profile->ReadObjectOuter(at);};
        }
    }
};
template<class T> void Store(uint8_t* base,size_t at,T value){std::memcpy(base+at,&value,sizeof(value));}
}

int main(){try {
    // Factory isolation: call order and mutations must not contaminate another target.
    for(bool preserve:{false,true})for(bool outline:{false,true}) {
        auto a=UE_DefaultOffsets::UE5_03(preserve,outline);
        auto b=UE_DefaultOffsets::UE5_03(!preserve,!outline);
        auto c=UE_DefaultOffsets::UE5_03(preserve,outline);
        Require(a.FName.Size==size_t((outline?4:8)+(preserve?4:0)),"FName factory parameters");
        Require(c.FName.Size==a.FName.Size && b.Config.isUsingCasePreservingName==!preserve,"factory call order isolation");
        a.FName.Size=99;Require(UE_DefaultOffsets::UE5_03(preserve,outline).FName.Size==c.FName.Size,"factory mutation isolation");
    }
    Require(!UE_DefaultOffsets::UE4_21(false).Config.IsUsingFNamePool,"UE4.21 uses legacy names");
    Require(!UE_DefaultOffsets::UE4_22(false).Config.IsUsingFNamePool,"UE4.22 uses legacy names");
    Require(UERuntime::ParseVersion("abc++UE5+Release-5.6-x").minor==6,"UE5 version branch");
    Require(UERuntime::ParseVersion("++UE4+Release-5.2").major==5,"UE5 in legacy branch marker");
    Require(UERuntime::ParseVersion("+Release-4.27").minor==27,"UE4 version branch");
    Require(!UERuntime::ParseVersion("++UE5+Release-5."),"bounded truncated version");
    Require(!UERuntime::ParseVersion("5.6.123 Android"),"unrelated number not version evidence");

    Memory m;auto read=m.reader();auto o=UE_DefaultOffsets::UE5_03(false,true);
    const uintptr_t pool=0x101000,block=0x110000;
    m.put(pool+o.FNamePool.BlocksOff,block);
    auto resolve=[&](uint32_t id){return UERuntime::PoolEntry(read,pool,id,o);};
    m.put<uint16_t>(block,4<<6);m.raw(block+2,"None",4);
    m.put<uint16_t>(block+8,12<<6);m.raw(block+10,"ByteProperty",12);
    auto get=[&](uintptr_t at){return UERuntime::Name(read,at,o,resolve);};
    Require(get(resolve(0))=="None","pool None");
    Require(get(resolve(4))=="ByteProperty","pool ByteProperty");
    // Numbered name payload is unaligned (+2), and ID zero is legal.
    m.put<uint16_t>(block+30,0);m.put<uint32_t>(block+32,4);m.put<uint32_t>(block+36,8);
    Require(get(block+30)=="ByteProperty_7","outline number suffix");
    m.put<uint32_t>(block+32,0);Require(get(block+30)=="None_7","outline references ID zero");
    m.put<uint32_t>(block+32,15);Require(get(block+30).empty(),"outline self cycle bounded");
    const char16_t wide[]=u"对象\U0001F600";
    m.put<uint16_t>(block+64,(4<<6)|1);m.raw(block+66,wide,8);
    Require(get(block+64)=="对象\xF0\x9F\x98\x80","UTF16 including surrogate pair");
    m.put<char16_t>(block+66,0xd800);m.put<char16_t>(block+68,'A');
    Require(get(block+64).empty(),"invalid UTF16 rejected");
    auto caseLayout=UE_DefaultOffsets::UE5_03(true,true);
    m.put<uint16_t>(block+100+4,4<<1);m.raw(block+106,"None",4);
    m.put<uint16_t>(block+128+4,0);m.put<uint32_t>(block+136,0);m.put<uint32_t>(block+140,3);
    Require(UERuntime::Name(read,block+128,caseLayout,[&](uint32_t){return block+100;})=="None_2","case-preserving numbered payload alignment");
    o.FNamePool.BlocksBit=64;Require(!UERuntime::PoolEntry(read,pool,4,o),"invalid shift bounded");
    o=UE_DefaultOffsets::UE5_03(false,false);
    Require(!UERuntime::PoolEntry(read,pool,0x7fff0000,o),"out of range block rejected");
    m.put<uint16_t>(0x17fffe,63<<6);Require(UERuntime::Name(read,0x17fffe,o,{}).empty(),"unreadable tail rejected");
    auto legacy=UE_DefaultOffsets::UE4_18_19(false);
    m.put<uint32_t>(block+200+legacy.FNameEntry.Index,1);m.raw(block+200+legacy.FNameEntry.Name,u"测试",6);
    Require(UERuntime::Name(read,block+200,legacy,{})=="测试","legacy UTF16 names");

    // Verified inverse-encoded fixture, independent of production decrypt helper.
    std::array<unsigned char,16> encrypted{0x3b,0xcf,0xcd,0xca,0x84,0x1d,0x32,0x34,0x7f,0x39,0x53,0xd5,0xc0,0xe4,0xe7,0x22};
    UERuntime::NewStateNames(encrypted.data(),encrypted.size());
    Require(std::string(reinterpret_cast<char*>(encrypted.data()),16)=="NoneByteProperty","New State ANSI known answer");
    char16_t wideEncrypted[]{char16_t(0x4e3b),char16_t(0x65cf),char16_t(0x67cd),char16_t(0x8bca)};
    UERuntime::NewStateNames(wideEncrypted,4);
    Require(wideEncrypted[0]==0x4e4e && wideEncrypted[1]==0x656f && wideEncrypted[2]==0x676e && wideEncrypted[3]==0x8b65,"New State UTF16 preserves high bytes");
    Require(UERuntime::NewStatePointer({1,0,3})==0,"zero encrypted components");
    const uintptr_t descriptor=0x102000;
    for(int32_t code:{INT32_MIN,0,100,102,151,INT32_MAX}) {m.put(descriptor,code);Require(!UERuntime::PubgChain(read,descriptor),"PUBG descriptor bound");}
    m.put<int32_t>(descriptor,103);m.put<uintptr_t>(descriptor+8,block);
    Require(UERuntime::PubgChain(read,descriptor)==block,"PUBG one hop");
    m.put<int32_t>(descriptor,106);m.put<uintptr_t>(descriptor+8,descriptor+0x100);m.put<uintptr_t>(descriptor+0x100,block);
    Require(UERuntime::PubgChain(read,descriptor)==block,"PUBG two hops");
    m.put<uintptr_t>(descriptor+0x100,descriptor+0x100);Require(!UERuntime::PubgChain(read,descriptor),"PUBG descriptor cycle");

    // UE4 TPair and UE5.6 tagged split arrays must produce identical sparse values.
    const uintptr_t header=0x103000,names=0x120000,values=0x124000;
    const auto name=[](uintptr_t at){return std::string("Enum::V")+std::to_string(at);};
    m.put<uintptr_t>(header,names);m.put<int32_t>(header+8,3);m.put<int32_t>(header+12,3);
    m.put<int64_t>(names+8,-1);m.put<int64_t>(names+24,42);m.put<int64_t>(names+40,42);
    auto pairs=UERuntime::EnumValues(read,header,8,name);
    Require(pairs.size()==3 && pairs[0].second==-1 && pairs[1].second==42 && pairs[2].second==42,"signed and aliased enums preserved");
    m.put<uintptr_t>(header,names|1);m.put<uintptr_t>(header+8,values|1);m.put<int32_t>(header+16,3);
    m.put<int64_t>(values,-1);m.put<int64_t>(values+8,42);m.put<int64_t>(values+16,42);
    auto split=UERuntime::EnumValues(read,header,4,name);
    Require(split.size()==3 && split[0].second==-1 && split[2].second==42,"UE5 split enum storage");
    m.put<int32_t>(header+16,70000);Require(UERuntime::EnumValues(read,header,4,name).empty(),"enum count bound");

    // Actual process-memory backend + actual wrappers/profile callbacks, not just pure helpers.
    auto* mem=static_cast<uint8_t*>(mmap(nullptr,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    Require(mem!=MAP_FAILED,"fixture mmap");
    Require(UEMemory::kMgr.initialize(getpid(),EK_MEM_OP_SYSCALL,false) || UEMemory::kMgr.initialize(getpid(),EK_MEM_OP_IO,false),"self memory reader");
    UEMemory::kPtrValidator.setPID(getpid());UEMemory::kPtrValidator.setUseCache(true);UEMemory::kPtrValidator.refreshRegionCache();
    NewStateProfile profile;auto offsets=UE_DefaultOffsets::UE5_03(false,false);
    offsets.UObject.InternalIndex=0x8;offsets.UObject.ClassPrivate=0x18;offsets.UObject.OuterPrivate=0x38;
    std::array<uint32_t,3> idx{uint32_t(77)^std::rotr(uint32_t(0x12345678^0x88776655^0xB5C76D21),7),0x12345678,0x88776655};
    std::memcpy(mem+8,idx.data(),sizeof(idx));
    auto encPtr=[&](size_t at,uintptr_t target){std::array<uint64_t,3> v{target^std::rotr(uint64_t(0x1111222233334444ULL^0x5555666677778888ULL^0xD2CA397CEA681802ULL),1),0x1111222233334444,0x5555666677778888};std::memcpy(mem+at,v.data(),sizeof(v));};
    encPtr(0x18,uintptr_t(mem+0x300));encPtr(0x38,uintptr_t(mem+0x500));
    Vars vars(offsets,&profile);UEWrappers::Init(&vars);
    Require(UE_UObject(mem).GetIndex()==77,"New State index through wrapper");
    Require(UE_UObject(mem).GetClass().GetAddress()==mem+0x300,"New State Class through wrapper");
    Require(UE_UObject(mem).GetOuter().GetAddress()==mem+0x500,"New State Outer through wrapper");
    offsets.Config.fieldOwnerUsesMask=true;Store(mem,offsets.FField.Owner,uintptr_t(mem+0x400)|1);
    Require(UE_FField(mem).GetOwnerPtr()==uintptr_t(mem+0x400),"UE5 tagged field owner");
    offsets.Config.structAlignmentIs16Bit=true;Store(mem,offsets.UStruct.MinAlignment,uint32_t(0x12340008));
    Require(UE_UStruct(mem).GetMinAlignment()==8,"UE5.6 uint16 alignment");
    offsets.Config.isUsingCasePreservingName=true;offsets.FName.DisplayIndex=4;offsets.FName.Number=8;
    Store(mem,0,uint32_t(1));Store(mem,4,uint32_t(2));Store(mem,8,uint32_t(4));
    Require(UE_FName(mem).GetName()=="DisplayName_3","display name and instance suffix");
    // A synthetic old NameArray behind multiple wrappers uses the real discovery code.
    std::memset(mem,0,0x10000);Probe probe;
    Store(mem,0x100,uintptr_t(mem+0x200));Store(mem,0x200,uintptr_t(mem+0x300));
    Store(mem,0x300,uintptr_t(mem+0x400));Store(mem,0x308,uintptr_t(mem+0x440));
    Store(mem,0x408,uint32_t(0));std::memcpy(mem+0x40c,"None",5);
    Store(mem,0x448,uint32_t(2));std::memcpy(mem+0x44c,"ByteProperty",13);
    Require(probe.ResolveLegacyTable(uintptr_t(mem+0x100))==uintptr_t(mem+0x200),"legacy table wrapper resolution");
    Require(probe.offsets.FNameEntry.Name==12,"legacy entry header detected");
    std::memset(mem+0x1000,0,64);Store(mem,0x1000,uint16_t(4<<6));std::memcpy(mem+0x1002,"None",4);
    Store(mem,0x1006,uint16_t(12<<6));std::memcpy(mem+0x1008,"ByteProperty",12);
    Require(probe.DetectNamePoolBlock(uintptr_t(mem+0x1000)),"standard pool block discovery");
    Store(mem,0x1006,uint16_t(7<<6));Require(!probe.DetectNamePoolBlock(uintptr_t(mem+0x1000)),"pool false positive rejected");
    Vars plain(offsets);UEWrappers::Init(&plain);Store(mem,offsets.UObject.InternalIndex,int32_t(123));
    Require(UE_UObject(mem).GetIndex()==123,"switching profile clears decryption callbacks");
    offsets=UE_DefaultOffsets::UE5_03(false,false);
    auto field=[&](size_t at,size_t classAt,uint32_t name,int32_t size) {
        Store(mem,at+offsets.FField.ClassPrivate,uintptr_t(mem+classAt));
        Store(mem,classAt+offsets.FFieldClass.Name,name);
        Store(mem,at+offsets.FProperty.ElementSize,size);
    };
    field(0x2000,0x2400,3,8);
    Require(UE_FProperty(mem+0x2000).GetType().second=="double","UE5 double property through wrappers");
    field(0x2800,0x2c00,4,16);
    Store(mem,0x2800+offsets.FProperty.Size,uintptr_t(mem+0x2000));
    Require(UE_FProperty(mem+0x2800).GetType().second=="TOptional<double>","UE5 optional type export");
    Store(mem,0x2800+offsets.FProperty.Size,uintptr_t(mem+0x2800));
    Require(UE_FProperty(mem+0x2800).GetType().first==UEPropertyType::Unknown,"optional self-cycle bounded");
    field(0x3000,0x3400,5,8);
    Require(UE_FProperty(mem+0x3000).GetType().second=="double","LWC 64-bit scalar");
    Store(mem,0x3000+offsets.FProperty.ElementSize,int32_t(4));
    Require(UE_FProperty(mem+0x3000).GetType().second=="float","LWC 32-bit scalar");
    // Pool discovery also handles case-preserving headers and encoded New State bytes.
    Store(mem,0x4004,uint16_t(4<<1));std::memcpy(mem+0x4006,"None",4);
    Store(mem,0x4010,uint16_t(12<<1));std::memcpy(mem+0x4012,"ByteProperty",12);
    Require(probe.DetectNamePoolBlock(uintptr_t(mem+0x4000)),"case-preserving pool discovery");
    Require(probe.offsets.FNamePool.Stride==4 && probe.offsets.FNamePoolEntry.Header==4,"case-preserving stride/header selected");
    UEWrappers::Init(nullptr);munmap(mem,0x10000);
    std::cout<<"PASS: "<<checks<<" engine compatibility checks (UE4/UE5 names, layouts, enums, PUBG and New State)\n";
    return 0;
} catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
