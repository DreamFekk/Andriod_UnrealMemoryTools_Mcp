#include "ExplorerApi.hpp"
#include "UE/UEWrappers.hpp"
#include "VectorValue.hpp"
#include <cmath>
#include <fmt/format.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utfcpp/unchecked.h>

namespace ExplorerApi {
namespace {
using json=nlohmann::json;
using namespace UEMemory;
using Clock=std::chrono::steady_clock;
thread_local Clock::time_point deadline;
thread_local size_t nestedValues=0;
bool TimeLeft() {return Clock::now()<deadline;}
std::string Hex(uintptr_t n) {return fmt::format("0x{:X}",n);}
uintptr_t Address(UE_UObject o) {return reinterpret_cast<uintptr_t>(o.GetAddress());}
template<class T> T ReadAt(uintptr_t address) {return vm_rpm_ptr<T>(reinterpret_cast<void*>(address));}
bool Readable(uintptr_t p) {return p && kPtrValidator.isPtrReadable(p,1);}
int Bounded(const json& r,const char* key,int fallback,int minimum,int maximum) {
    if(!r.contains(key)) return fallback;
    if(!r[key].is_number_integer()) throw std::runtime_error(std::string(key)+" must be an integer.");
    return int(std::clamp<int64_t>(r[key].get<int64_t>(),minimum,maximum));
}
uintptr_t ParseAddress(const json& r) {
    if(!r.contains("address") || !r["address"].is_string()) throw std::runtime_error("address must be a hexadecimal string.");
    auto text=r["address"].get<std::string>();
    if(text.starts_with("0x") || text.starts_with("0X")) text.erase(0,2);
    uintptr_t address=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),address,16);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !Readable(address))
        throw std::runtime_error("Invalid or unreadable address.");
    return address;
}
std::string Lower(std::string s) {for(char& c:s) if(c>='A' && c<='Z') c+=32;return s;}
std::string FullName(UE_UObject obj) {
    std::string path=obj.GetName();
    std::unordered_set<uintptr_t> seen{Address(obj)};
    for(auto outer=obj.GetOuter();outer && seen.size()<64;outer=outer.GetOuter()) {
        if(!seen.insert(Address(outer)).second) break;
        path=outer.GetName()+"."+path;
    }
    return obj.GetClass().GetName()+" "+path;
}
bool Is(UE_UObject object,const char* className) {
    std::unordered_set<uintptr_t> seen;
    for(auto cls=object.GetClass().Cast<UE_UStruct>();cls && seen.size()<64;cls=cls.GetSuper()) {
        if(!seen.insert(Address(cls)).second) break;
        if(cls.GetName()==className) return true;
    }
    return false;
}
std::string CppName(UE_UObject obj) {
    if(!obj) return "Unknown";
    if(!Is(obj,"Class")) return "F"+obj.GetName();
    std::unordered_set<uintptr_t> seen;
    for(auto cls=obj.Cast<UE_UStruct>();cls && seen.size()<64;cls=cls.GetSuper()) {
        if(!seen.insert(Address(cls)).second) break;
        if(cls.GetName()=="Actor") return "A"+obj.GetName();
    }
    return "U"+obj.GetName();
}
bool ValidObject(UE_UObject o) {
    if(!Readable(Address(o))) return false;
    auto cls=o.GetClass();
    return Readable(Address(cls)) && o.GetIndex()>=0 && !cls.GetName().empty();
}
json Identity(UE_UObject o) {
    return {{"index",o.GetIndex()},{"address",Hex(Address(o))},{"name",o.GetName()},
            {"fullName",FullName(o)},{"className",o.GetClass().GetName()}};
}
struct Field {
    uintptr_t address=0;
    bool modern=false;
    std::string name,kind,type,owner;
    uintptr_t ownerAddress=0;
    int32_t offset=0,size=0,dimension=0;
    uint64_t flags=0;
    json typeLinks=json::array();
};
uintptr_t Derived(const Field& f,uintptr_t resolved,uintptr_t extra=0) {
    const auto* o=UEWrappers::GetOffsets();
    uintptr_t offset=f.modern?resolved:0;
    if(!offset) {
        offset=f.modern?o->FProperty.Size:o->UProperty.Size;
        if(f.modern && !Readable(ReadAt<uintptr_t>(f.address+offset)) && Readable(ReadAt<uintptr_t>(f.address+offset+8))) offset+=8;
        offset+=extra;
    }
    return offset?ReadAt<uintptr_t>(f.address+offset):0;
}
Field Describe(uintptr_t address,bool modern,int depth=0) {
    Field f;f.address=address;f.modern=modern;
    if(!Readable(address) || depth>8) {f.type="unknown";return f;}
    if(modern) {
        UE_FProperty p(reinterpret_cast<uint8_t*>(address));
        f.name=p.GetName();f.kind=p.GetClass().GetName();f.offset=p.GetOffset();f.size=p.GetSize();f.dimension=p.GetArrayDim();f.flags=p.GetPropertyFlags();
    } else {
        UE_UProperty p(reinterpret_cast<uint8_t*>(address));
        f.name=p.GetName();f.kind=p.GetClass().GetName();f.offset=p.GetOffset();f.size=p.GetSize();f.dimension=p.GetArrayDim();f.flags=p.GetPropertyFlags();
    }
    if(f.kind=="ObjectPtrProperty") f.kind="ObjectProperty";
    else if(f.kind=="ClassPtrProperty") f.kind="ClassProperty";
    else if(f.kind=="LargeWorldCoordinatesRealProperty" && (f.size==4 || f.size==8)) f.kind=f.size==8?"DoubleProperty":"FloatProperty";
    static const std::unordered_map<std::string,std::string> scalars={
        {"FloatProperty","float"},{"DoubleProperty","double"},{"BoolProperty","bool"},{"ByteProperty","uint8_t"},
        {"IntProperty","int32_t"},{"Int32Property","int32_t"},{"Int8Property","int8_t"},{"Int16Property","int16_t"},
        {"Int64Property","int64_t"},{"UInt16Property","uint16_t"},{"UInt32Property","uint32_t"},{"UInt64Property","uint64_t"},
        {"NameProperty","FName"},{"StrProperty","FString"},{"TextProperty","FText"},{"DelegateProperty","FScriptDelegate"},
        {"MulticastDelegateProperty","FMulticastScriptDelegate"},{"FieldPathProperty","FFieldPath"}};
    auto scalar=scalars.find(f.kind);
    f.type=scalar==scalars.end()?f.kind:scalar->second;
    const auto* o=UEWrappers::GetOffsets();
    auto link=[&](uintptr_t p,const std::string& label) {
        UE_UObject target(reinterpret_cast<uint8_t*>(p));
        if(ValidObject(target)) f.typeLinks.push_back({{"name",label},{"address",Hex(p)},{"kind","object"}});
    };
    auto named=[&](uintptr_t p) {
        const auto name=CppName(UE_UObject(reinterpret_cast<uint8_t*>(p)));
        link(p,name);return name;
    };
    auto nested=[&](uintptr_t resolved,uintptr_t extra=0){
        auto inner=Describe(Derived(f,resolved,extra),modern,depth+1);
        for(const auto& item:inner.typeLinks) f.typeLinks.push_back(item);
        return inner.type;
    };
    if(f.kind=="OptionalProperty") f.type="TOptional<"+nested(o->FProperty.Size)+">";
    else if(f.kind=="ArrayProperty") f.type="TArray<"+nested(o->ArrayProperty.Inner)+">";
    else if(f.kind=="SetProperty") f.type="TSet<"+nested(o->SetProperty.ElementProp)+">";
    else if(f.kind=="MapProperty") f.type="TMap<"+nested(o->MapProperty.KeyProp)+", "+nested(o->MapProperty.ValueProp,8)+">";
    else if(f.kind=="StructProperty") f.type=named(Derived(f,o->StructProperty.Struct));
    else if(f.kind=="ObjectProperty" || f.kind=="ClassProperty") f.type=named(Derived(f,f.kind=="ClassProperty"?o->ClassProperty.MetaClass:o->ObjectProperty.PropertyClass,f.kind=="ClassProperty"?8:0))+"*";
    else if(f.kind=="SoftObjectProperty" || f.kind=="WeakObjectProperty" || f.kind=="LazyObjectProperty" || f.kind=="SoftClassProperty") {
        const std::string wrapper=f.kind=="WeakObjectProperty"?"TWeakObjectPtr":f.kind=="LazyObjectProperty"?"TLazyObjectPtr":f.kind=="SoftClassProperty"?"TSoftClassPtr":"TSoftObjectPtr";
        f.type=wrapper+"<"+named(Derived(f,o->ObjectProperty.PropertyClass))+">";
    } else if(f.kind=="ByteProperty" || f.kind=="EnumProperty") {
        UE_UObject e(reinterpret_cast<uint8_t*>(Derived(f,f.kind=="ByteProperty"?o->ByteProperty.Enum:o->EnumProperty.Enum,f.kind=="EnumProperty"?8:0)));
        if(e && Is(e,"Enum")) {f.type="enum "+e.GetName();link(Address(e),e.GetName());}
        else if(f.kind=="EnumProperty") f.type=nested(o->EnumProperty.UnderlayingProp);
    } else if(f.kind=="InterfaceProperty") {
        UE_UObject target(reinterpret_cast<uint8_t*>(Derived(f,o->InterfaceProperty.InterfaceClass)));
        f.type="TScriptInterface<I"+target.GetName()+">";
        link(Address(target),"I"+target.GetName());
    }
    return f;
}
std::vector<Field> Fields(UE_UStruct structure,bool inherit=true) {
    std::vector<UE_UStruct> ancestry;
    std::unordered_set<uintptr_t> seen;
    for(auto s=structure;s && ancestry.size()<32 && TimeLeft();s=inherit?s.GetSuper():UE_UStruct()) {
        if(!seen.insert(Address(s)).second) break;
        ancestry.push_back(s);
    }
    std::reverse(ancestry.begin(),ancestry.end());
    std::vector<Field> fields;
    for(const auto& s:ancestry) {
        seen.clear();
        for(auto f=s.GetChildProperties();f && fields.size()<2048 && seen.size()<4096 && TimeLeft();f=f.GetNext()) {
            auto address=reinterpret_cast<uintptr_t>(f.GetAddress());
            if(!seen.insert(address).second) break;
            auto row=Describe(address,true);row.owner=s.GetName();row.ownerAddress=Address(s);fields.push_back(std::move(row));
        }
        seen.clear();
        for(auto f=s.GetChildren();f && fields.size()<2048 && seen.size()<4096 && TimeLeft();f=f.GetNext()) {
            if(!seen.insert(Address(f)).second) break;
            if(Is(f,"Property")) {auto row=Describe(Address(f),false);row.owner=s.GetName();row.ownerAddress=Address(s);fields.push_back(std::move(row));}
        }
    }
    return fields;
}
json Header(uintptr_t address) {
    auto h=ReadAt<TArray<uint8_t>>(address);
    return {{"address",Hex(address)},{"dataAddress",Hex(reinterpret_cast<uintptr_t>(h.GetData()))},{"data",Hex(reinterpret_cast<uintptr_t>(h.GetData()))},
            {"count",h.Num()},{"max",h.Max()},{"capacity",h.Max()},{"valid",h.Num()>=0 && h.Max()>=h.Num() && h.Max()<=16*1024*1024 && (!h.Num() || Readable(reinterpret_cast<uintptr_t>(h.GetData())))}};
}
json Value(uintptr_t address,const Field& f,int depth=0) {
    json v={{"value",""},{"typeLinks",f.typeLinks}};const auto& k=f.kind;
    if(!Readable(address)) {v["value"]="<unreadable>";return v;}
    if(f.size>0 && f.size<=8) {
        std::array<uint8_t,8> test{};
        if(!vm_rpm_ptr(reinterpret_cast<void*>(address),test.data(),f.size)) {v["value"]="<unreadable>";return v;}
    }
    if(k=="StructProperty") {
        UE_UStruct structure(reinterpret_cast<uint8_t*>(Derived(f,UEWrappers::GetOffsets()->StructProperty.Struct)));
        const auto name=structure.GetName();const auto size=structure.GetSize();
        v["structAddress"]=Hex(Address(structure));v["structName"]=name;
        const int width=VectorValue::Width(name,size>0?size:0);
        if(width && f.size==size) {
            std::array<uint8_t,24> bytes{};
            if(!vm_rpm_ptr(reinterpret_cast<void*>(address),bytes.data(),width*3)) {v["value"]="<unreadable FVector>";return v;}
            const auto values=VectorValue::Decode(bytes.data(),width);
            v["value"]=VectorValue::Format(values,width);v["valueKind"]="vector3";v["scalarType"]=width==4?"float32":"float64";
            v["components"]=json::object();
            for(int i=0;i<3;++i) {
                const char* axis=i==0?"X":i==1?"Y":"Z";
                v["components"][axis]=std::isfinite(values[i])?json(values[i]):json(fmt::format("{}",values[i]));
            }
            return v;
        }
        v["value"]=f.type+" {…}";v["children"]=json::array();v["valueKind"]="struct";
        if(depth>=2 || size<=0 || size>f.size || nestedValues>=256) {v["childrenTruncated"]=true;return v;}
        auto fields=Fields(structure);
        size_t emitted=0;
        for(const auto& child:fields) {
            if(!TimeLeft() || nestedValues>=256 || emitted>=32) break;
            if(child.offset<0 || child.size<=0 || int64_t(child.offset)+child.size>size ||
               uintptr_t(child.offset)>std::numeric_limits<uintptr_t>::max()-address) continue;
            ++nestedValues;++emitted;
            const auto at=address+uintptr_t(child.offset);
            auto row=Value(at,child,depth+1);
            row.update({{"name",child.name},{"type",child.type},{"offset",Hex(child.offset)},{"address",Hex(at)},
                        {"owner",child.owner},{"size",child.size}});
            v["children"].push_back(std::move(row));
        }
        v["childrenTruncated"]=emitted<fields.size();
    }
    else if(k=="FloatProperty") v["value"]=fmt::format("{:.9g}",ReadAt<float>(address));
    else if(k=="DoubleProperty") v["value"]=fmt::format("{:.6f}",ReadAt<double>(address));
    else if(k=="IntProperty" || k=="Int32Property") v["value"]=std::to_string(ReadAt<int32_t>(address));
    else if(k=="Int8Property") v["value"]=std::to_string(ReadAt<int8_t>(address));
    else if(k=="Int16Property") v["value"]=std::to_string(ReadAt<int16_t>(address));
    else if(k=="Int64Property") v["value"]=std::to_string(ReadAt<int64_t>(address));
    else if(k=="UInt16Property") v["value"]=std::to_string(ReadAt<uint16_t>(address));
    else if(k=="UInt32Property") v["value"]=std::to_string(ReadAt<uint32_t>(address));
    else if(k=="UInt64Property") v["value"]=std::to_string(ReadAt<uint64_t>(address));
    else if(k=="ByteProperty") v["value"]=std::to_string(ReadAt<uint8_t>(address));
    else if(k=="BoolProperty") {
        const auto* o=UEWrappers::GetOffsets();
        const uintptr_t b=f.address+(f.modern?(o->BoolProperty.Base?o->BoolProperty.Base:o->FProperty.Size):o->UProperty.Size);
        auto byte=ReadAt<uint8_t>(b+1),mask=ReadAt<uint8_t>(b+3);
        v["value"]=mask?((ReadAt<uint8_t>(address+byte)&mask)?"true":"false"):"<invalid bool mask>";
    } else if(k=="NameProperty") v["value"]=UE_FName(reinterpret_cast<uint8_t*>(address)).GetName();
    else if(k=="StrProperty") {
        auto h=ReadAt<TArray<uint8_t>>(address);std::string text;
        if(h.Num()>=0 && h.Num()<=65536 && h.Max()>=h.Num() && h.GetData()) {
            const auto utf16=vm_rpm_strw(h.GetData(),std::min(h.Num(),4096));
            for(size_t i=0;i<utf16.size();++i) {
                uint32_t cp=uint16_t(utf16[i]);
                if(cp>=0xD800 && cp<=0xDBFF) {
                    if(i+1<utf16.size() && uint16_t(utf16[i+1])>=0xDC00 && uint16_t(utf16[i+1])<=0xDFFF)
                        cp=0x10000+((cp-0xD800)<<10)+(uint16_t(utf16[++i])-0xDC00);
                    else cp=0xFFFD;
                } else if(cp>=0xDC00 && cp<=0xDFFF) cp=0xFFFD;
                utf8::unchecked::append(cp,std::back_inserter(text));
            }
        }
        v["value"]=text;
    } else if(k=="ObjectProperty" || k=="ClassProperty" || k=="InterfaceProperty") {
        const auto p=ReadAt<uintptr_t>(address);v["pointer"]=Hex(p);
        UE_UObject object(reinterpret_cast<uint8_t*>(p));v["value"]=p?Hex(p)+(ValidObject(object)?" "+object.GetName():""):"nullptr";
    } else if(k=="WeakObjectProperty") {
        int32_t index=ReadAt<int32_t>(address),serial=ReadAt<int32_t>(address+4);
        v["value"]=fmt::format("Index={} Serial={}",index,serial);
        if(index>=0 && index<UEWrappers::GetObjects()->GetNumElements()) {
            const auto p=reinterpret_cast<uintptr_t>(UEWrappers::GetObjects()->GetObjectPtr(index));
            if(p) {v["candidatePointer"]=Hex(p);v["note"]="Weak object serial has not been verified.";}
        }
    } else if(k=="ArrayProperty" || k=="SetProperty" || k=="MapProperty") {
        auto h=Header(address);v["value"]=fmt::format("Count={} Max={} Data={}",h["count"].get<int>(),h["max"].get<int>(),h["data"].get<std::string>());
    } else if(k=="EnumProperty" && (f.size==1 || f.size==2 || f.size==4 || f.size==8)) {
        uint64_t n=0;vm_rpm_ptr(reinterpret_cast<void*>(address),&n,f.size);v["value"]=std::to_string(n);
    } else v["value"]=Hex(ReadAt<uintptr_t>(address));
    return v;
}
json Function(UE_UFunction fn) {
    std::string returns="void",parameters;
    for(const auto& p:Fields(fn,false)) {
        if(!(p.flags&CPF_Parm)) continue;
        if(p.flags&CPF_ReturnParm) {returns=p.type;continue;}
        if(!parameters.empty()) parameters+=", ";
        parameters+=(p.flags&CPF_ConstParm?"const ":"")+p.type+((p.flags&(CPF_OutParm|CPF_ReferenceParm))?"&":"")+" "+p.name;
    }
    const auto native=fn.GetFunc(),base=UEWrappers::GetBaseAddress();
    return {{"name",fn.GetName()},{"address",Hex(native)},{"objectAddress",Hex(Address(fn))},
            {"owner",fn.GetOuter().GetName()},{"ownerAddress",Hex(Address(fn.GetOuter()))},
            {"rva",Hex(native>=base?native-base:0)},{"flags",fn.GetFunctionFlags()},
            {"numParams",uint8_t(fn.GetNumParams())},{"paramSize",uint16_t(fn.GetParamSize())},
            {"signature",returns+" "+fn.GetName()+"("+parameters+")"}};
}
json List(const json& request) {
    const int total=std::clamp(UEWrappers::GetObjects()->GetNumElements(),0,4*1024*1024);
    int cursor=Bounded(request,"cursor",0,0,total),scanned=0;
    const int limit=Bounded(request,"limit",100,1,1000),scanLimit=Bounded(request,"scanLimit",4096,1,4096);
    const auto query=Lower(request.value("query",std::string{}));
    if(query.size()>512) throw std::runtime_error("Search query exceeds 512 bytes.");
    json items=json::array();
    while(cursor<total && scanned<scanLimit && items.size()<size_t(limit) && TimeLeft()) {
        UE_UObject obj(UEWrappers::GetObjects()->GetObjectPtr(cursor++));++scanned;
        if(!ValidObject(obj)) continue;
        const auto name=FullName(obj);
        if(!query.empty() && Lower(name).find(query)==std::string::npos) continue;
        auto item=Identity(obj);item["fullName"]=name;items.push_back(std::move(item));
    }
    return {{"generation",UEWrappers::GetInitGeneration()},{"total",total},{"items",std::move(items)},
            {"nextCursor",cursor},{"scanned",scanned},{"complete",cursor>=total}};
}json Container(uintptr_t base,const Field& f,int maxRows) {
    const uintptr_t address=base+f.offset;
    json result=Header(address);result.update({{"name",f.name},{"type",f.type},{"kind",f.kind},{"items",json::array()}});
    if(f.kind!="ArrayProperty") {
        result["note"]="Raw header preview only: sparse Set/Map allocation layout is not decoded; count/max are raw words.";
        return result;
    }
    const auto inner=Describe(Derived(f,UEWrappers::GetOffsets()->ArrayProperty.Inner),f.modern);
    result["elementSize"]=inner.size;
    if(!result.value("valid",false) || inner.size<=0 || inner.size>16*1024*1024) return result;
    const auto h=ReadAt<TArray<uint8_t>>(address);
    const auto data=reinterpret_cast<uintptr_t>(h.GetData());
    const int rows=std::min(h.Num(),maxRows);
    for(int i=0;i<rows && TimeLeft();++i) {
        if(uintptr_t(i)> (std::numeric_limits<uintptr_t>::max()-data)/uintptr_t(inner.size)) break;
        const auto element=data+uintptr_t(i)*inner.size;
        auto item=Value(element,inner);item.update({{"index",i},{"address",Hex(element)},{"type",inner.type}});
        result["items"].push_back(std::move(item));
    }
    result["truncated"]=result["items"].size()<size_t(std::max(0,h.Num()));
    return result;
}
uintptr_t MemberAddress(UE_UObject object,const std::vector<Field>& fields,const char* name) {
    for(const auto& field:fields)
        if(field.name==name && field.offset>=0 && field.size>0 && int64_t(field.offset)+field.size<=object.GetClass().GetSize())
            return Address(object)+field.offset;
    return 0;
}
bool Registered(UE_UObject object) {
    return ValidObject(object) && UEWrappers::GetObjects()->GetObjectPtr(object.GetIndex())==object.GetAddress();
}
json Worlds(const json& request) {
    const int total=std::clamp(UEWrappers::GetObjects()->GetNumElements(),0,4*1024*1024);
    int cursor=Bounded(request,"cursor",0,0,total),scanned=0;
    const int limit=Bounded(request,"limit",64,1,256),budget=Bounded(request,"scanLimit",8192,1,16384);
    std::unordered_map<uintptr_t,bool> classes;
    json items=json::array();
    while(cursor<total && scanned<budget && items.size()<size_t(limit) && TimeLeft()) {
        UE_UObject object(UEWrappers::GetObjects()->GetObjectPtr(cursor++));++scanned;
        if(!ValidObject(object))continue;
        const auto cls=Address(object.GetClass());
        auto [entry,fresh]=classes.try_emplace(cls,false);
        if(fresh)entry->second=Is(object,"World");
        if(!entry->second || object.GetName().starts_with("Default__"))continue;
        const auto fields=Fields(object.GetClass());
        const auto slot=MemberAddress(object,fields,"PersistentLevel");
        UE_UObject level(reinterpret_cast<uint8_t*>(slot?ReadAt<uintptr_t>(slot):0));
        if(!Registered(level) || !Is(level,"Level"))continue;
        const auto gi=MemberAddress(object,fields,"OwningGameInstance"),levels=MemberAddress(object,fields,"Levels");
        const auto instance=gi?ReadAt<uintptr_t>(gi):0;
        auto row=Identity(object);
        row.update({{"persistentLevel",Hex(Address(level))},{"gameInstance",Hex(instance)},
                    {"hasGameInstance",Readable(instance)},{"levelCount",levels?Header(levels)["count"]:json(0)}});
        items.push_back(std::move(row));
    }
    return {{"generation",UEWrappers::GetInitGeneration()},{"items",items},{"nextCursor",cursor},
            {"scanned",scanned},{"total",total},{"complete",cursor>=total}};
}
struct ActorLevel {uintptr_t address=0,data=0;int32_t count=0,max=0;std::string name;};
json Actors(const json& request) {
    const auto worldAddress=ParseAddress(json{{"address",request.value("worldAddress",std::string{})}});
    UE_UObject world(reinterpret_cast<uint8_t*>(worldAddress));
    if(!Registered(world) || !Is(world,"World") || world.GetName().starts_with("Default__"))
        throw std::runtime_error("Select a live UWorld instance, not its class/default object.");
    const auto fields=Fields(world.GetClass());
    const auto persistentSlot=MemberAddress(world,fields,"PersistentLevel"),levelsSlot=MemberAddress(world,fields,"Levels");
    const auto actorsOffset=UEWrappers::GetOffsets()->ULevel.Actors;
    if(!actorsOffset || actorsOffset>0x1000000)throw std::runtime_error("ULevel.Actors offset is not available.");
    std::vector<uintptr_t> addresses;
    if(persistentSlot)addresses.push_back(ReadAt<uintptr_t>(persistentSlot));
    json warnings=json::array();
    if(levelsSlot) {
        const auto header=Header(levelsSlot);
        if(header.value("valid",false) && header["count"].get<int>()<=2048) {
            const auto count=header["count"].get<int>();
            const auto data=ReadAt<uintptr_t>(levelsSlot);
            std::vector<uintptr_t> loaded(count);
            if(count && !vm_rpm_ptr(reinterpret_cast<void*>(data),loaded.data(),loaded.size()*sizeof(uintptr_t)))
                warnings.push_back("World.Levels pointer array is unreadable.");
            else addresses.insert(addresses.end(),loaded.begin(),loaded.end());
        }else warnings.push_back("World.Levels header is invalid or exceeds 2048 levels.");
    }else warnings.push_back("World.Levels is not reflected; only PersistentLevel is available.");
    std::unordered_set<uintptr_t> seen;
    std::vector<ActorLevel> levels;
    json levelRows=json::array();
    const auto selected=request.value("levelAddress",std::string{});
    const auto selectedAddress=selected.empty()?0:ParseAddress(json{{"address",selected}});
    uint64_t hash=1469598103934665603ull;
    auto mix=[&](uint64_t n){hash^=n;hash*=1099511628211ull;};
    mix(UEWrappers::GetInitGeneration());mix(worldAddress);mix(world.GetIndex());
    int64_t total=0;
    for(auto address:addresses) {
        if(!address || !seen.insert(address).second)continue;
        if(!TimeLeft())throw std::runtime_error("Level enumeration reached its time budget; retry or choose a smaller World.");
        UE_UObject level(reinterpret_cast<uint8_t*>(address));
        if(!Registered(level) || !Is(level,"Level")){warnings.push_back("A Level pointer is stale or unreadable.");continue;}
        auto h=Header(address+actorsOffset);
        const bool valid=h.value("valid",false) && h["count"].get<int>()<=1024*1024;
        levelRows.push_back({{"address",Hex(address)},{"name",FullName(level)},{"count",h["count"]},
                            {"valid",valid},{"actorsHeader",Hex(address+actorsOffset)},{"actorsData",h["data"]}});
        if(!valid){warnings.push_back("Invalid Actors header at "+Hex(address));continue;}
        ActorLevel entry{address,ReadAt<uintptr_t>(address+actorsOffset),h["count"].get<int>(),h["max"].get<int>(),FullName(level)};
        if(selectedAddress && selectedAddress!=address)continue;
        mix(address);mix(entry.data);mix(entry.count);mix(entry.max);
        total+=entry.count;levels.push_back(std::move(entry));
    }
    if(!selected.empty() && levels.empty())throw std::runtime_error("Selected Level is no longer loaded; refresh the World.");
    if(total>4*1024*1024)throw std::runtime_error("Actor slot count exceeds the bounded traversal limit.");
    const auto topology=Hex(hash);
    if(request.contains("expectedTopology") && request["expectedTopology"]!=topology)
        return {{"error","Level/Actors arrays changed; restart this page scan."},{"changed",true},{"generation",UEWrappers::GetInitGeneration()},{"topology",topology}};
    const int cursor=Bounded(request,"cursor",0,0,int(total));
    int position=cursor,scanned=0,invalid=0;
    const int limit=Bounded(request,"limit",100,1,1000),budget=Bounded(request,"scanLimit",4096,1,16384);
    const auto query=Lower(request.value("query",std::string{}));
    if(query.size()>512)throw std::runtime_error("Actor query exceeds 512 bytes.");
    json items=json::array();
    int begin=0;
    std::unordered_set<uintptr_t> actorsSeen;
    std::array<uintptr_t,256> pointers{};
    for(const auto& level:levels) {
        const int end=begin+level.count;
        if(position>=end){begin=end;continue;}
        int local=std::max(0,position-begin);
        while(local<level.count && scanned<budget && items.size()<size_t(limit) && TimeLeft()) {
            const int count=std::min({int(pointers.size()),level.count-local,budget-scanned});
            const auto slot=level.data+uintptr_t(local)*sizeof(uintptr_t);
            if(!vm_rpm_ptr(reinterpret_cast<void*>(slot),pointers.data(),count*sizeof(uintptr_t))) {
                local+=count;position=begin+local;scanned+=count;invalid+=count;
                warnings.push_back("Actor pointer block unreadable in "+Hex(level.address));continue;
            }
            for(int i=0;i<count && items.size()<size_t(limit) && TimeLeft();++i) {
                const int actorSlot=local++;position=begin+local;++scanned;
                UE_UObject actor(reinterpret_cast<uint8_t*>(pointers[i]));
                if(!Registered(actor) || !Is(actor,"Actor") || !actorsSeen.insert(Address(actor)).second){++invalid;continue;}
                auto row=Identity(actor);
                if(!query.empty() && Lower(row["fullName"].get<std::string>()).find(query)==std::string::npos)continue;
                row.update({{"levelAddress",Hex(level.address)},{"levelName",level.name},{"actorSlot",actorSlot},
                            {"slotAddress",Hex(level.data+uintptr_t(actorSlot)*sizeof(uintptr_t))}});
                items.push_back(std::move(row));
            }
        }
        if(scanned>=budget || items.size()>=size_t(limit) || !TimeLeft())break;
        begin=end;
    }
    // Bypass this request's page cache to detect a resized/replaced Actors
    // array during traversal. Actor contents can still change in a live game.
    for(const auto& level:levels) {
        struct HeaderNow {uintptr_t data;int32_t count,max;} now{};
        if(kMgr.readMem(level.address+actorsOffset,&now,sizeof(now))!=sizeof(now) ||
           now.data!=level.data || now.count!=level.count || now.max!=level.max)
            return {{"error","Actors array changed during traversal; restart this page scan."},{"changed",true},{"generation",UEWrappers::GetInitGeneration()}};
    }
    return {{"generation",UEWrappers::GetInitGeneration()},{"world",Identity(world)},{"levels",levelRows},
            {"actorsOffset",Hex(actorsOffset)},{"topology",topology},{"items",items},{"totalSlots",total},
            {"nextCursor",position},{"scanned",scanned},{"invalidSlots",invalid},{"complete",position>=total},
            {"warnings",warnings},{"sampledAtMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
}
json Runtime(UE_UObject object,const std::vector<Field>& fields,int maxRows) {
    json runtime={{"kind","Object"},{"fields",json::array()},{"actors",json::array()}};
    auto find=[&](const char* name)->const Field* {for(const auto& field:fields) if(field.name==name) return &field;return nullptr;};
    auto objectField=[&](const char* name)->uintptr_t {
        auto field=find(name);
        if(!field) {runtime["fields"].push_back({{"name",name},{"value","<missing>"}});return 0;}
        const auto address=Address(object)+field->offset,ptr=ReadAt<uintptr_t>(address);
        UE_UObject target(reinterpret_cast<uint8_t*>(ptr));
        runtime["fields"].push_back({{"name",name},{"address",Hex(address)},{"pointer",Hex(ptr)},
                                      {"value",ValidObject(target)?FullName(target):Hex(ptr)}});
        return ptr;
    };
    uintptr_t level=0;
    if(Is(object,"World")) {
        runtime["kind"]="UWorld";level=objectField("PersistentLevel");objectField("OwningGameInstance");
        objectField(find("NetDriver")?"NetDriver":"DemoNetDriver");
    } else if(Is(object,"Level")) {runtime["kind"]="ULevel";level=Address(object);}
    else if(Is(object,"DataTable")) {
        runtime["kind"]="UDataTable";objectField("RowStruct");
        const auto offset=UEWrappers::GetOffsets()->UDataTable.RowMap;
        if(offset) {
            runtime["rowMap"]=Header(Address(object)+offset);
            runtime["rowMap"]["note"]="Raw RowMap header; sparse map rows are not decoded (same scope as the native view).";
        }
    }
    const auto actorOffset=UEWrappers::GetOffsets()->ULevel.Actors;
    if(level && actorOffset) {
        const auto header=Header(level+actorOffset);runtime["actorCount"]=header["count"];runtime["actorMax"]=header["max"];
        if(header.value("valid",false)) {
            auto actors=ReadAt<TArray<uint8_t*>>(level+actorOffset);
            for(int i=0;i<std::min(actors.Num(),maxRows) && TimeLeft();++i) {
                UE_UObject actor(ReadAt<uint8_t*>(reinterpret_cast<uintptr_t>(actors.GetData())+uintptr_t(i)*8));
                if(!ValidObject(actor)) continue;
                auto item=Identity(actor);item["index"]=i;runtime["actors"].push_back(std::move(item));
            }
        }
    }
    return runtime;
}
json InspectField(uintptr_t address) {
    UE_FField field(reinterpret_cast<uint8_t*>(address));
    auto cls=field.GetClass();
    if(!Readable(reinterpret_cast<uintptr_t>(cls.GetAddress()))) throw std::runtime_error("Address does not contain a readable FField class.");
    const auto kind=cls.GetName(),name=field.GetName();
    if(kind.empty() || name.empty()) throw std::runtime_error("Address is not a recognized FField.");
    json result={{"generation",UEWrappers::GetInitGeneration()},{"kind","field"},{"address",Hex(address)},{"index",-1},
        {"name",name},{"fullName",kind+" "+name},{"className",kind},{"classAddress",Hex(reinterpret_cast<uintptr_t>(cls.GetAddress()))},
        {"outerAddress",Hex(field.GetOwnerPtr())},{"superAddress",Hex(reinterpret_cast<uintptr_t>(cls.GetSuperClass().GetAddress()))},
        {"metadata",json::array()},{"properties",json::array()},{"functions",json::array()},{"containers",json::array()},
        {"runtime",{{"kind","FField"},{"fields",json::array()},{"actors",json::array()}}}};
    auto meta=[&](const char* key,const std::string& value){result["metadata"].push_back({{"name",key},{"value",value}});};
    meta("Owner (raw FFieldVariant)",Hex(field.GetOwnerPtr()));meta("Next",Hex(reinterpret_cast<uintptr_t>(field.GetNext().GetAddress())));
    meta("CastFlags",Hex(cls.GetCastFlags()));
    if(kind.ends_with("Property")) {
        const auto f=Describe(address,true);
        if(f.offset<0 || f.offset>0x10000000 || f.size<0 || f.size>0x1000000 || f.dimension<0 || f.dimension>0x10000)
            throw std::runtime_error("FProperty layout validation failed.");
        meta("Type",f.type);meta("Offset",Hex(f.offset));meta("ElementSize",std::to_string(f.size));
        meta("ArrayDim",std::to_string(f.dimension));meta("PropertyFlags",Hex(f.flags));
    }
    return result;
}
json Inspect(const json& request) {
    auto address=ParseAddress(request);
    const auto source=address;
    const bool dereference=request.value("dereference",false);
    if(dereference && (!vm_rpm_ptr(reinterpret_cast<void*>(source),&address,sizeof(address)) || !Readable(address)))
        throw std::runtime_error("Pointer slot is unreadable or points to null/unmapped memory.");
    const auto kind=request.value("kind",std::string{"object"});
    if(kind=="field") return InspectField(address);
    if(kind=="struct") {
        const auto type=ParseAddress(json{{"address",request.value("typeAddress",std::string{})}});
        UE_UStruct structure(reinterpret_cast<uint8_t*>(type));
        if(!ValidObject(structure) || !Is(structure,"ScriptStruct")) throw std::runtime_error("typeAddress must point to a reflected UScriptStruct definition.");
        const int size=structure.GetSize();
        if(size<=0 || size>64*1024*1024 || uintptr_t(size)>std::numeric_limits<uintptr_t>::max()-address)
            throw std::runtime_error("Invalid struct size or address range.");
        const auto identity=Hex(address)+":"+Hex(type)+":"+structure.GetName();
        if(request.contains("expectedIdentity") && request["expectedIdentity"]!=identity)
            throw std::runtime_error("Structure pointer/type changed; inspect it explicitly again.");
        json result={{"kind","struct"},{"address",Hex(address)},{"typeAddress",Hex(type)},{"classAddress",Hex(type)},
            {"sourceAddress",Hex(dereference?source:address)},{"dereferenced",dereference},{"identity",identity},
            {"name",structure.GetName()},{"className",CppName(structure)},{"fullName",CppName(structure)+" @ "+Hex(address)},
            {"generation",UEWrappers::GetInitGeneration()},{"sampledAtMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
            {"metadata",json::array({{{"name","Size"},{"value",Hex(size)}},{{"name","Type definition"},{"value",Hex(type)}},
                                    {{"name","Storage"},{"value",dereference?"dereferenced pointer":"inline/direct address"}}})},
            {"properties",json::array()},{"functions",json::array()},{"containers",json::array()},{"runtime",json::object()},
            {"note","Offsets are relative to this structure address. A raw structure has no UObject identity/lifetime guarantee."}};
        const int maxRows=Bounded(request,"maxRows",64,1,4096);
        const auto fields=Fields(structure);
        for(const auto& f:fields) {
            if(!TimeLeft()) break;
            if(f.offset<0 || f.size<=0 || f.dimension<=0 || int64_t(f.offset)+int64_t(f.size)*f.dimension>size) continue;
            const auto at=address+f.offset;
            auto row=Value(at,f);row.update({{"name",f.name},{"type",f.type},{"kind",f.kind},{"offset",Hex(f.offset)},
                {"address",Hex(at)},{"size",int64_t(f.size)*f.dimension},{"owner",f.owner},{"descriptorAddress",Hex(f.address)},
                {"descriptorKind",f.modern?"field":"object"}});
            result["properties"].push_back(std::move(row));
            if(f.kind=="ArrayProperty" || f.kind=="SetProperty" || f.kind=="MapProperty") result["containers"].push_back(Container(address,f,maxRows));
        }
        result["truncated"]=!TimeLeft();
        return result;
    }
    if(kind!="object" && kind!="instance") throw std::runtime_error("kind must be object, instance, struct or field.");
    UE_UObject object(reinterpret_cast<uint8_t*>(address));
    if(!ValidObject(object)) throw std::runtime_error("Address is not a readable UObject; use field mode for FField/FProperty.");
    const bool declaration=Is(object,"Struct");
    if(kind=="instance" && declaration) throw std::runtime_error("This is a class/struct definition, not an instance. Use UObject mode for definitions.");
    const int index=object.GetIndex();
    if(index<0 || index>=UEWrappers::GetObjects()->GetNumElements() ||
       reinterpret_cast<uintptr_t>(UEWrappers::GetObjects()->GetObjectPtr(index))!=address)
        throw std::runtime_error("Object is no longer present in the object array; refresh its instance address.");
    const int maxRows=Bounded(request,"maxRows",64,1,4096);
    auto result=Identity(object);auto cls=object.GetClass();auto outer=object.GetOuter();
    const auto identity=Hex(address)+":"+std::to_string(index)+":"+Hex(Address(cls))+":"+object.GetName();
    if(request.contains("expectedIdentity") && request["expectedIdentity"]!=identity)
        throw std::runtime_error("Instance identity changed; stop monitoring and inspect the new object explicitly.");
    const auto structure=Is(object,"Struct")?object.Cast<UE_UStruct>():cls.Cast<UE_UStruct>();
    result.update({{"generation",UEWrappers::GetInitGeneration()},{"kind",kind},{"classAddress",Hex(Address(cls))},
        {"isInstance",!declaration},{"identity",identity},{"classHierarchy",json::array()},
        {"sourceAddress",Hex(dereference?source:address)},{"dereferenced",dereference},
        {"sampledAtMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
        {"outerAddress",Hex(Address(outer))},{"superAddress",Hex(structure?Address(structure.GetSuper()):0)},
        {"metadata",json::array()},{"properties",json::array()},{"functions",json::array()},
        {"containers",json::array()},{"enums",json::array()}});
    auto meta=[&](const char* key,const std::string& value){result["metadata"].push_back({{"name",key},{"value",value}});};
    meta("Address",Hex(address));meta("VFTable",Hex(ReadAt<uintptr_t>(address)));
    meta("ObjectFlags",Hex(uint32_t(object.GetFlags())));meta("InternalIndex",std::to_string(object.GetIndex()));
    meta("NamePrivate",Hex(address+UEWrappers::GetOffsets()->UObject.NamePrivate));
    meta("ClassPrivate",Hex(Address(cls))+" "+cls.GetName());meta("OuterPrivate",Hex(Address(outer))+" "+outer.GetName());
    meta("CppName",CppName(object));meta("FullName",FullName(object));
    if(structure) {
        meta("Struct",FullName(structure));meta("Super",FullName(structure.GetSuper()));
        meta("Size",Hex(structure.GetSize()));meta("Alignment",std::to_string(structure.GetMinAlignment()));
        meta("ChildProperties",Hex(reinterpret_cast<uintptr_t>(structure.GetChildProperties().GetAddress())));
        meta("Children",Hex(Address(structure.GetChildren())));
    }
    const auto metaClass=Is(object,"Class")?object.Cast<UE_UClass>():cls;
    result["classDefaultObjectAddress"]=Hex(Address(metaClass.GetClassDefaultObject()));
    meta("ClassDefaultObject",Hex(Address(metaClass.GetClassDefaultObject())));
    meta("ImplementedInterfaces",Hex(metaClass.GetImplementedInterfacesPtr()));
    meta("CastFlags",Hex(metaClass.GetCastFlags()));meta("ClassFlags",Hex(metaClass.GetClassFlags()));
    if(Is(object,"ScriptStruct")) meta("StructFlags",Hex(object.Cast<UE_UScriptStruct>().GetStructFlags()));
    if(Is(object,"Function")) result["functions"].push_back(Function(object.Cast<UE_UFunction>()));
    else {
        std::unordered_set<uintptr_t> seen,types;
        for(auto owner=structure;owner && types.size()<32 && TimeLeft();owner=owner.GetSuper()) {
            if(!types.insert(Address(owner)).second) break;
            result["classHierarchy"].push_back({{"name",owner.GetName()},{"address",Hex(Address(owner))},{"size",owner.GetSize()}});
            for(auto child=owner.GetChildren();child && seen.size()<8192 && result["functions"].size()<1024 && TimeLeft();child=child.GetNext()) {
                if(!seen.insert(Address(child)).second) break;
                if(Is(child,"Function")) {
                    auto row=Function(child.Cast<UE_UFunction>());row["inherited"]=Address(owner)!=Address(structure);
                    result["functions"].push_back(std::move(row));
                }
            }
        }
    }
    // A selected class/struct also exposes its declared field descriptors.
    const auto declarations=Fields(structure);
    for(const auto& f:declarations) {
        result["properties"].push_back({{"name",f.name},{"type",f.type},{"kind",f.kind},{"offset",Hex(uint32_t(f.offset))},
            {"size",int64_t(f.size)*f.dimension},{"address",Hex(f.address)},{"descriptorAddress",Hex(f.address)},
            {"descriptorKind",f.modern?"field":"object"},{"owner",f.owner},{"ownerAddress",Hex(f.ownerAddress)},
            {"typeLinks",f.typeLinks},{"flags",Hex(f.flags)},{"valueKind","declaration"},{"value","<field descriptor>"}});
    }
    const auto liveFields=declaration?Fields(cls.Cast<UE_UStruct>()):declarations;
    if(!declaration) result["properties"]=json::array();
    json liveProperties=json::array();
    for(const auto& f:liveFields) {
        if(!TimeLeft()) break;
        if(f.offset<0 || f.size<=0 || f.dimension<=0 || f.offset>0x10000000 ||
           int64_t(f.offset)+int64_t(f.size)*f.dimension>cls.GetSize() ||
           uintptr_t(f.offset)>std::numeric_limits<uintptr_t>::max()-address) continue;
        const auto at=address+uintptr_t(f.offset);
        auto p=Value(at,f);p.update({{"name",f.name},{"type",f.type},{"kind",f.kind},{"offset",Hex(f.offset)},
            {"size",int64_t(f.size)*f.dimension},{"address",Hex(at)},{"descriptorAddress",Hex(f.address)},
            {"descriptorKind",f.modern?"field":"object"},{"owner",f.owner},{"ownerAddress",Hex(f.ownerAddress)},{"flags",Hex(f.flags)}});
        if(f.dimension>1) p["note"]="Fixed array: first element preview.";
        liveProperties.push_back(std::move(p));
        if(f.kind=="ArrayProperty" || f.kind=="SetProperty" || f.kind=="MapProperty") result["containers"].push_back(Container(address,f,maxRows));
    }
    if(declaration) result["objectProperties"]=std::move(liveProperties);else result["properties"]=std::move(liveProperties);
    if(Is(object,"Enum")) {
        const auto entries=object.Cast<UE_UEnum>().GetEntries(size_t(maxRows));
        for(const auto& [name,value]:entries) result["enums"].push_back({{"name",name},{"value",std::to_string(value)}});
        meta("EnumEntries",std::to_string(object.Cast<UE_UEnum>().GetEntryCount()));
        if(entries.empty()) result["note"]="No readable enum entries; this build's storage may require another enum layout.";
    }
    result["runtime"]=Runtime(object,liveFields,maxRows);
    result["truncated"]=!TimeLeft();
    if(!TimeLeft()) result["note"]="Inspection reached its time budget; lower maxRows or inspect a nested object directly.";
    return result;
}
} // namespace
json Read(const json& request) {
    if(!request.is_object()) return {{"error","Expected a JSON object."}};
    try {
        const auto generation=UEWrappers::GetInitGeneration();
        if(request.contains("generation") && request["generation"]!=generation)
            return {{"error","Probe generation changed; refresh the object browser."},{"generation",generation}};
        const auto action=request.value("action",std::string{"list"});
        deadline=Clock::now()+std::chrono::milliseconds(action=="list"?100:500);nestedValues=0;
        if(action=="list") return List(request);
        if(action=="worlds") return Worlds(request);
        if(action=="actors") return Actors(request);
        if(action=="inspect") return Inspect(request);
        return {{"error","Unknown explorer action."},{"generation",generation}};
    } catch(const std::exception& e) {return {{"error",e.what()},{"generation",UEWrappers::GetInitGeneration()}};}
}
std::string PropertyValue(uintptr_t instance,uintptr_t descriptor,bool modern) {
    deadline=Clock::now()+std::chrono::milliseconds(10);nestedValues=0;
    const auto field=Describe(descriptor,modern);
    if(field.offset<0 || field.offset>0x10000000 || uintptr_t(field.offset)>std::numeric_limits<uintptr_t>::max()-instance) return "<invalid offset>";
    return Value(instance+field.offset,field).value("value",std::string{});
}
} // namespace ExplorerApi
