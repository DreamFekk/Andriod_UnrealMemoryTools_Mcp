#include "App.hpp"
#include "Dumper.hpp"
#include "FastDump.hpp"
#include "ExplorerApi.hpp"
#include "Utils/Logger.hpp"
#include "UE/UEMemory.hpp"
#include "UE/UEGameProfiles/AutoFix.hpp"
#include "UE/UEGameProfiles/ArenaBreakout.hpp"
#include "UE/UEGameProfiles/DeltaForce.hpp"
#include "UE/UEGameProfiles/Farlight.hpp"
#include "UE/UEGameProfiles/NRC.hpp"
#include "UE/UEGameProfiles/PUBGMHD.hpp"
#include "UE/UEGameProfiles/PUBG.hpp"
#include "UE/UEGameProfiles/NewState.hpp"
#include "UE/UEGameProfiles/Sfps2.hpp"
#include "UE/UEGameProfiles/Valorant.hpp"
#include "UE/UEGameProfiles/WutheringWaves.hpp"
#ifdef UMT_NATIVE_UI
#include "SDKExplorer.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>

namespace App {
namespace {
using namespace UEMemory;
using Clock = std::chrono::steady_clock;
std::mutex stateMutex, actionMutex, operationMutex;
std::thread worker;
std::string outputRoot;
json state;
struct ProbeOffsetEntry { std::string name; uintptr_t value=0,relative=0; bool found=false; };
struct StructFieldEntry { std::string name,type; uintptr_t offset=0; bool found=false; std::string description; };
struct StructGroup { std::string name; std::vector<StructFieldEntry> fields; };
struct Session {
    pid_t pid=0;
    std::string package, birth;
    std::unique_ptr<IGameProfile> profile;
} session;

std::vector<std::unique_ptr<IGameProfile>> Profiles() {
    std::vector<std::unique_ptr<IGameProfile>> p;
    p.emplace_back(std::make_unique<ArenaBreakoutProfile>());
    p.emplace_back(std::make_unique<DeltaForceProfile>());
    p.emplace_back(std::make_unique<FarlightProfile>());
    p.emplace_back(std::make_unique<ShuishaProfile>());
    p.emplace_back(std::make_unique<ValorantProfile>());
    p.emplace_back(std::make_unique<NRCProfile>());
    p.emplace_back(std::make_unique<PUBGMHDProfile>());
    p.emplace_back(std::make_unique<PUBGProfile>());
    p.emplace_back(std::make_unique<NewStateProfile>());
    p.emplace_back(std::make_unique<WutheringWavesProfile>());
    return p;
}
std::string Hex(uintptr_t value) { return fmt::format("0x{:X}", value); }
std::string UrlEncode(const std::string& value) {
    static constexpr char hex[]="0123456789ABCDEF";
    std::string encoded;
    for(unsigned char c:value) {
        if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='~') encoded+=char(c);
        else {encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}
    }
    return encoded;
}
std::string Birth(pid_t pid) {
    std::ifstream f("/proc/"+std::to_string(pid)+"/stat");
    std::string line,part;
    if (!std::getline(f,line)) return {};
    auto close=line.rfind(')');
    if (close==std::string::npos) return {};
    std::istringstream fields(line.substr(close+2));
    for (int number=3;number<=22;++number) if (!(fields>>part)) return {};
    return part;
}
void Log(char level,const char* message) {
    std::lock_guard lock(stateMutex);
    auto& logs=state["logs"];
    if (logs.size()>=1500) logs.erase(logs.begin(),logs.begin()+100);
    logs.push_back(std::string(1,level)+": "+(message?message:""));
}
void Update(const json& patch) { std::lock_guard lock(stateMutex); state.update(patch); }
void Phase(const char* phase) { Update({{"phase",phase}}); }
void EnsureDirectory(const std::string& path) {
    if (IOUtils::mkdir_recursive(path,0775)==-1 && errno!=EEXIST)
        throw std::runtime_error("Cannot create output directory: "+path+": "+strerror(errno));
}
bool SafeName(const std::string& name) {
    return !name.empty() && name!="." && name!=".." && name.find_first_of("/\\") == std::string::npos && name.find('\0')==std::string::npos;
}
void CheckSession(pid_t pid) {
    if (!session.profile || session.pid!=pid) throw std::runtime_error("Probe the selected process first.");
    if (session.birth.empty() || Birth(pid)!=session.birth) {
        Update({{"probed",false}});
        throw std::runtime_error("Target exited or PID was reused; probe again.");
    }
}
    std::vector<ProbeOffsetEntry> CollectProbeOffsets(IGameProfile *profile)
    {
        std::vector<ProbeOffsetEntry> offsets;
        if (!profile)
            return offsets;

        const auto *vars = profile->GetUEVars();
        if (!vars)
            return offsets;

        const uintptr_t base = vars->GetBaseAddress();
        auto add = [&](const char *name, uintptr_t value)
        {
            ProbeOffsetEntry e;
            e.name = name;
            e.value = value;
            e.relative = (value && base && value >= base) ? (value - base) : 0;
            e.found = value != 0;
            offsets.push_back(std::move(e));
        };

        add("BaseAddress", base);
        add("GUObjectArray", vars->GetGUObjectsArrayPtr());
        add("ObjObjects", vars->GetObjObjectsPtr());
        add(profile->IsUsingFNamePool() ? "FNamePool" : "GNames", vars->GetNamesPtr());
        add("ProcessEvent", vars->GetProcessEvent());
        add("StaticFindObject", vars->GetStaticFindObject());
        add("FrameCount", vars->GetFrameCount());
        add("Matrix", vars->GetMatrix());
        add("Physx", vars->GetPhysx());
        add("NativeAndroidApp", vars->GetNativeAndroidApp());
        return offsets;
    }

    std::vector<StructGroup> CollectStructGroups(IGameProfile *profile)
    {
        std::vector<StructGroup> groups;
        if (!profile)
            return groups;

        const auto *vars = profile->GetUEVars();
        if (!vars)
            return groups;

        UE_Offsets *off = vars->GetOffsets();
        if (!off)
            return groups;

        auto pushField = [](StructGroup &g, const char *name, const char *type,
                            uintptr_t offset, const char *desc, bool allowZero = false)
        {
            StructFieldEntry e;
            e.name = name;
            e.type = type;
            e.offset = offset;
            e.found = allowZero ? true : (offset != 0);
            e.description = desc ? desc : "";
            g.fields.push_back(std::move(e));
        };

        // 基础指针组
        {
            StructGroup g;
            g.name = "基础";
            const uintptr_t base = vars->GetBaseAddress();
            auto pushAbs = [&](const char *name, const char *type, uintptr_t value, const char *desc)
            {
                StructFieldEntry e;
                e.name = name;
                e.type = type;
                e.offset = (value && base && value >= base) ? (value - base) : value;
                e.found = value != 0;
                e.description = desc;
                g.fields.push_back(std::move(e));
            };
            pushAbs("BaseAddress", "uintptr_t", base, "可执行模块基址");
            pushAbs("GUObjectArray", "FUObjectArray*", vars->GetGUObjectsArrayPtr(), "全局对象数组指针 (相对基址)");
            pushAbs("ObjObjects", "FChunkedFixedUObjectArray*", vars->GetObjObjectsPtr(), "ObjObjects 指针 (相对基址)");
            pushAbs(profile->IsUsingFNamePool() ? "FNamePool" : "GNames",
                    profile->IsUsingFNamePool() ? "FNamePool*" : "TStaticIndirectArrayThreadSafeRead*",
                    vars->GetNamesPtr(), "名称池/全局名称表 (相对基址)");
            pushAbs("ProcessEvent", "void*", vars->GetProcessEvent(), "ProcessEvent 函数地址 (相对基址)");
            pushAbs("StaticFindObject", "void*", vars->GetStaticFindObject(), "StaticFindObject 函数地址 (相对基址)");
            pushAbs("FrameCount", "uint64*", vars->GetFrameCount(), "GFrameCounter 地址 (相对基址)");
            pushAbs("Matrix", "FMatrix*", vars->GetMatrix(), "ViewProjection 矩阵地址 (相对基址)");
            pushAbs("Physx", "void*", vars->GetPhysx(), "PhysX 全局指针 (相对基址)");
            pushAbs("NativeAndroidApp", "android_app*", vars->GetNativeAndroidApp(), "GNativeAndroidApp (相对基址)");
            groups.push_back(std::move(g));
        }

        // UObject
        {
            StructGroup g;
            g.name = "UObject";
            pushField(g, "VTable", "void*", 0, "虚函数表 (固定 0)", true);
            pushField(g, "ObjectFlags", "EObjectFlags", off->UObject.ObjectFlags, "对象标志", true);
            pushField(g, "InternalIndex", "int32", off->UObject.InternalIndex, "GUObjectArray 中的索引", true);
            pushField(g, "ClassPrivate", "UClass*", off->UObject.ClassPrivate, "对象所属类");
            pushField(g, "NamePrivate", "FName", off->UObject.NamePrivate, "对象名称");
            pushField(g, "OuterPrivate", "UObject*", off->UObject.OuterPrivate, "外部 (Outer) 对象");
            groups.push_back(std::move(g));
        }

        // UField
        {
            StructGroup g;
            g.name = "UField";
            pushField(g, "Next", "UField*", off->UField.Next, "链表下一个 UField");
            groups.push_back(std::move(g));
        }

        // UStruct
        {
            StructGroup g;
            g.name = "UStruct";
            pushField(g, "SuperStruct", "UStruct*", off->UStruct.SuperStruct, "父结构 (基类)");
            pushField(g, "Children", "UField*", off->UStruct.Children, "子字段链表");
            pushField(g, "ChildProperties", "FField*", off->UStruct.ChildProperties, "子属性链表 (UE5)");
            pushField(g, "PropertiesSize", "int32", off->UStruct.PropertiesSize, "属性总大小");
            pushField(g, "MinAlignment", "int32", off->UStruct.MinAlignment, "最小对齐");
            groups.push_back(std::move(g));
        }

        // UClass
        {
            StructGroup g;
            g.name = "UClass";
            pushField(g, "(继承自 UStruct)", "-", 0, "UClass 复用 UStruct 全部偏移", true);
            pushField(g, "ClassDefaultObject", "UObject*", off->UClass.ClassDefaultObject, "类默认对象");
            pushField(g, "ImplementedInterfaces", "TArray<FImplementedInterface>", off->UClass.ImplementedInterfaces, "接口实现列表");
            pushField(g, "CastFlags", "uint64", off->UClass.CastFlags, "类 CastFlags");
            pushField(g, "ClassFlags", "uint32", off->UClass.ClassFlags, "类标志");
            groups.push_back(std::move(g));
        }

        // UScriptStruct
        {
            StructGroup g;
            g.name = "UScriptStruct";
            pushField(g, "(继承自 UStruct)", "-", 0, "UScriptStruct 复用 UStruct 布局", true);
            pushField(g, "StructFlags", "uint32", off->UScriptStruct.StructFlags, "脚本结构标志");
            groups.push_back(std::move(g));
        }

        // UFunction
        {
            StructGroup g;
            g.name = "UFunction";
            pushField(g, "EFunctionFlags", "EFunctionFlags", off->UFunction.EFunctionFlags, "函数标志");
            pushField(g, "NumParams", "uint8", off->UFunction.NumParams, "参数个数");
            pushField(g, "ParamSize", "uint16", off->UFunction.ParamSize, "参数总大小");
            pushField(g, "Func", "void*", off->UFunction.Func, "函数指针 (Native)");
            groups.push_back(std::move(g));
        }

        // FField + FProperty (UE 4.25+ / UE5)
        {
            StructGroup g;
            g.name = "FField+FProperty";
            pushField(g, "FField.ClassPrivate", "FFieldClass*", off->FField.ClassPrivate, "FField 所属类");
            pushField(g, "FField.Owner", "FFieldVariant", off->FField.Owner, "FField 所属者");
            pushField(g, "FField.Next", "FField*", off->FField.Next, "下一个 FField");
            pushField(g, "FField.NamePrivate", "FName", off->FField.NamePrivate, "FField 名称");
            pushField(g, "FField.FlagsPrivate", "EObjectFlags", off->FField.FlagsPrivate, "FField 标志");
            pushField(g, "FFieldClass.Name", "FName", off->FFieldClass.Name, "FFieldClass 名称");
            pushField(g, "FFieldClass.SuperClass", "FFieldClass*", off->FFieldClass.SuperClass, "FFieldClass 父类");
            pushField(g, "FFieldClass.CastFlags", "uint64", off->FFieldClass.CastFlags, "FFieldClass CastFlags");
            pushField(g, "FProperty.ArrayDim", "int32", off->FProperty.ArrayDim, "数组维度");
            pushField(g, "FProperty.ElementSize", "int32", off->FProperty.ElementSize, "单元素大小");
            pushField(g, "FProperty.PropertyFlags", "uint64", off->FProperty.PropertyFlags, "属性标志");
            pushField(g, "FProperty.Offset_Internal", "int32", off->FProperty.Offset_Internal, "属性内部偏移");
            pushField(g, "FProperty.Size", "size_t", off->FProperty.Size, "FProperty 总大小");
            pushField(g, "UProperty.ArrayDim", "int32", off->UProperty.ArrayDim, "(旧版) 数组维度");
            pushField(g, "UProperty.ElementSize", "int32", off->UProperty.ElementSize, "(旧版) 单元素大小");
            pushField(g, "UProperty.PropertyFlags", "uint64", off->UProperty.PropertyFlags, "(旧版) 属性标志");
            pushField(g, "UProperty.Offset_Internal", "int32", off->UProperty.Offset_Internal, "(旧版) 属性内部偏移");
            pushField(g, "UProperty.Size", "size_t", off->UProperty.Size, "(旧版) UProperty 总大小");
            groups.push_back(std::move(g));
        }

        // FName / FNameEntry / FNamePool
        {
            StructGroup g;
            g.name = "FName";
            pushField(g, "FName.ComparisonIndex", "int32", off->FName.ComparisonIndex, "比较索引", true);
            pushField(g, "FName.DisplayIndex", "int32", off->FName.DisplayIndex, "显示索引");
            pushField(g, "FName.Number", "int32", off->FName.Number, "Number 字段");
            pushField(g, "FName.Size", "size_t", off->FName.Size, "FName 结构大小");
            pushField(g, "FNameEntry.Index", "int32", off->FNameEntry.Index, "FNameEntry 索引");
            pushField(g, "FNameEntry.Name", "char/wchar*", off->FNameEntry.Name, "FNameEntry 字符串");
            pushField(g, "FNamePool.Stride", "uint32", off->FNamePool.Stride, "Pool 步长");
            pushField(g, "FNamePool.BlocksBit", "uint32", off->FNamePool.BlocksBit, "Block 位宽");
            pushField(g, "FNamePool.BlocksOff", "uint32", off->FNamePool.BlocksOff, "Block 起始偏移");
            pushField(g, "FNamePoolEntry.Header", "uint16", off->FNamePoolEntry.Header, "PoolEntry 头");
            groups.push_back(std::move(g));
        }

        {
            StructGroup g;
            g.name = "ULevel+UDataTable";
            pushField(g, "ULevel.Actors", "TArray<AActor*>", off->ULevel.Actors, "关卡中的 Actor 列表");
            pushField(g, "UDataTable.RowMap", "TMap<FName,uint8*>", off->UDataTable.RowMap, "数据表行映射");
            groups.push_back(std::move(g));
        }

        // FUObjectArray / TUObjectArray / FUObjectItem
        {
            StructGroup g;
            g.name = "ObjectArray";
            pushField(g, "FUObjectArray.ObjObjects", "FChunkedFixedUObjectArray", off->FUObjectArray.ObjObjects, "ObjObjects 偏移", true);
            pushField(g, "TUObjectArray.Objects", "void**", off->TUObjectArray.Objects, "对象指针表", true);
            pushField(g, "TUObjectArray.NumElements", "int32", off->TUObjectArray.NumElements, "元素数量");
            pushField(g, "TUObjectArray.NumElementsPerChunk", "int32", off->TUObjectArray.NumElementsPerChunk, "每块元素数量");
            pushField(g, "FUObjectItem.Object", "UObject*", off->FUObjectItem.Object, "对象指针", true);
            pushField(g, "FUObjectItem.Size", "size_t", off->FUObjectItem.Size, "ObjectItem 大小");
            groups.push_back(std::move(g));
        }

        // UEnum
        {
            StructGroup g;
            g.name = "UEnum";
            pushField(g, "Names", "TArray<TPair<FName,int64>>", off->UEnum.Names, "枚举名称数组");
            groups.push_back(std::move(g));
        }

        return groups;
    }

void Probe(pid_t pid) {
    const std::string package=KittyMemoryEx::getProcessName(pid),birth=Birth(pid);
    if (!SafeName(package) || birth.empty()) throw std::runtime_error("Target process is unavailable.");
    // The operation lock excludes every explorer read while profile memory is replaced.
    UEWrappers::Init(nullptr);
    session=Session{};
    Update({{"probed",false},{"pid",pid},{"package",package},{"profile","Auto"},
            {"offsets",json::array()},{"groups",json::array()},{"resultPath",""},{"soDumpPath",""}});
    Phase("Initializing memory");
    if (!kMgr.initialize(pid,EK_MEM_OP_SYSCALL,false) && !kMgr.initialize(pid,EK_MEM_OP_IO,false))
        throw std::runtime_error("Cannot initialize process memory.");
    session.pid=pid; session.package=package; session.birth=birth;
    UEDumper dumper;
    auto profiles=Profiles();
    for (auto& profile:profiles) {
        const auto ids=profile->GetAppIDs();
        if (std::find(ids.begin(),ids.end(),package)==ids.end()) continue;
        Phase("Probing dedicated profile");
        LOGI("Profile: %s",profile->GetAppName().c_str());
        if (dumper.Init(profile.get())) session.profile=std::move(profile);
        else {
            UEWrappers::Init(nullptr);
            if(package=="com.pubg.newstate")
                throw std::runtime_error("New State profile validation failed: "+dumper.GetLastError()+". Reference keys/layout may differ from this build; plain AutoFix cannot decode encrypted objects.");
            LOGW("Dedicated profile failed: %s; using AutoFix.",dumper.GetLastError().c_str());
        }
        break;
    }
    if (!session.profile) {
        Phase("Probing AutoFix");
        auto profile=std::make_unique<AutoFixProfile>(package);
        if (!dumper.Init(profile.get())) {
            UEWrappers::Init(nullptr);
            throw std::runtime_error(dumper.GetLastError());
        }
        session.profile=std::move(profile);
    }
    CheckSession(pid);
    json offsets=json::array(),groups=json::array();
    for (const auto& item:CollectProbeOffsets(session.profile.get()))
        offsets.push_back({{"name",item.name},{"value",Hex(item.value)},{"relative",Hex(item.relative)},{"found",item.found}});
    for (const auto& group:CollectStructGroups(session.profile.get())) {
        json fields=json::array();
        for (const auto& item:group.fields)
            fields.push_back({{"name",item.name},{"type",item.type},{"offset",Hex(item.offset)},{"found",item.found},{"description",item.description}});
        groups.push_back({{"name",group.name},{"fields",std::move(fields)}});
    }
    Update({{"probed",true},{"generation",UEWrappers::GetInitGeneration()},{"profile",session.profile->GetAppName()},
            {"offsets",std::move(offsets)},{"groups",std::move(groups)}});
    LOGI("Probe complete: %s pid=%d",package.c_str(),pid);
}

void SaveBuffers(const std::unordered_map<std::string,BufferFmt>& buffers,const std::string& directory) {
    static constexpr const char banner[]="// 创作者: 曦曦(DreamFekk) https://github.com/DreamFekk\n// 禁止圈钱盗卖\n\n";
    for (const auto& [name,buffer]:buffers) {
        if (!SafeName(name)) throw std::runtime_error("Invalid output filename.");
        const auto path=directory+"/"+name,temp=path+".partial";
        FILE* f=fopen(temp.c_str(),"wb");
        if (!f) throw std::runtime_error("Cannot write "+temp);
        setvbuf(f,nullptr,_IOFBF,1024*1024);
        bool ok=true;
        if (name.size()<5 || name.substr(name.size()-5)!=".json")
            ok=fwrite(banner,1,sizeof(banner)-1,f)==sizeof(banner)-1;
        const auto data=buffer.readView();
        ok=ok && fwrite(data.data(),1,data.size(),f)==data.size();
        const int closed=fclose(f);
        if (!ok || closed || rename(temp.c_str(),path.c_str())) {
            unlink(temp.c_str());
            throw std::runtime_error("Failed saving "+path);
        }
    }
}
void Dump(pid_t pid) {
    CheckSession(pid);
    const auto directory=outputRoot+"/"+session.package;
    EnsureDirectory(directory);
    UEDumper dumper;
    dumper.setDumpExeInfoNotify([](bool done){if(!done) Phase("Executable information");});
    dumper.setDumpNamesInfoNotify([](bool done){if(!done) Phase("Names");});
    dumper.setDumpObjectsInfoNotify([](bool done){if(!done) Phase("Objects");});
    dumper.setDumpOffsetsInfoNotify([](bool done){if(!done) Phase("Engine offsets");});
    dumper.setObjectsProgressCallback([](const SimpleProgressBar& p){
        std::lock_guard lock(stateMutex);
        int percent=p.getPercentage();
        if (state["objectsPercent"]!=percent) state["objectsPercent"]=percent;
    });
    dumper.setDumpProgressCallback([](const SimpleProgressBar& p){
        std::lock_guard lock(stateMutex);
        int percent=p.getPercentage();
        if (state["dumpPercent"]!=percent) state["dumpPercent"]=percent;
    });
    if (!dumper.Init(session.profile.get(),true)) throw std::runtime_error(dumper.GetLastError());
    std::unordered_map<std::string,BufferFmt> buffers;
    const bool ok=dumper.Dump(&buffers);
    CheckSession(pid);
    Phase("Saving SDK");
    SaveBuffers(buffers,directory);
    if (!ok) throw std::runtime_error(dumper.GetLastError().empty()?"SDK generation failed.":dumper.GetLastError());
    Update({{"resultPath",directory},{"objectsPercent",100},{"dumpPercent",100}});
    LOGI("SDK saved: %s",directory.c_str());
}
void Library(pid_t pid) {
    CheckSession(pid);
    const auto elf=session.profile->GetUnrealELF();
    if (!elf.isValid() || elf.end()<=elf.base()) throw std::runtime_error("Unreal ELF is invalid.");
    auto name=IOUtils::get_filename(elf.filePath());
    if (!SafeName(name)) name="libUE4.so";
    const auto directory=outputRoot+"/"+session.package;
    EnsureDirectory(directory);
    const auto path=directory+"/"+name;
    Phase("Dumping library");
    const auto result=FastDump::Range(pid,elf.base(),elf.end(),path,[](uint64_t done,uint64_t total){
        Update({{"dumpPercent",total?int(done*100/total):0}});
    });
    if (!result.ok) throw std::runtime_error(result.error);
    CheckSession(pid);
    {
        std::lock_guard lock(stateMutex);
        state["metrics"]["libraryBytes"]=result.bytes;
        state["metrics"]["libraryReadCalls"]=result.readCalls;
        state["metrics"]["libraryMs"]=result.milliseconds;
        state["metrics"]["libraryMiBps"]=result.milliseconds>0?result.bytes/1048576.0/(result.milliseconds/1000.0):0;
    }
    Update({{"soDumpPath",path},{"resultPath",directory},{"dumpPercent",100}});
    LOGI("Library saved: %llu bytes in %.2f ms",(unsigned long long)result.bytes,result.milliseconds);
}
} // namespace

void Init(const std::string& root) {
    std::lock_guard actionLock(actionMutex);
    outputRoot=root;
    while(outputRoot.size()>1 && outputRoot.back()=='/') outputRoot.pop_back();
    if (outputRoot.empty()) outputRoot="/sdcard/UnrealMemoryTools_Refactored";
    {
        std::lock_guard lock(stateMutex);
        state={{"busy",false},{"phase","Ready"},{"error",""},{"package",""},{"pid",0},{"generation",0},{"outputRoot",outputRoot},
               {"profile",""},{"probed",false},{"objectsPercent",0},{"dumpPercent",0},
               {"resultPath",""},{"soDumpPath",""},{"logs",json::array()},
               {"offsets",json::array()},{"groups",json::array()},{"metrics",json::object()}};
    }
    Logger::SetSink(Log);
}
json Processes() {
    json result=json::array();
    std::unordered_map<std::string,std::string> names;
    for (const auto& profile:Profiles()) for(const auto& id:profile->GetAppIDs()) names[id]=profile->GetAppName();
    DIR* directory=opendir("/proc");
    if (!directory) return result;
    while(auto* entry=readdir(directory)) {
        char* end=nullptr;
        long number=strtol(entry->d_name,&end,10);
        if(!end || *end || number<=0) continue;
        pid_t pid=static_cast<pid_t>(number);
        const auto package=KittyMemoryEx::getProcessName(pid);
        if(package.empty()) continue;
        const auto found=names.find(package);
        bool unreal=found!=names.end();
        if(!unreal) for(const auto& map:KittyMemoryEx::getAllMaps(pid)) {
            if(map.pathname.find("libUE4.so")!=std::string::npos || map.pathname.find("libUnreal.so")!=std::string::npos) {unreal=true;break;}
        }
        if(unreal) {
            const std::string profile=found!=names.end()?found->second:"AutoFix (UE4/UE5)";
            result.push_back({{"pid",pid},{"package",package},{"profile",profile},{"profileName",profile},{"dedicated",found!=names.end()}});
        }
    }
    closedir(directory);
    std::sort(result.begin(),result.end(),[](const json& a,const json& b){return a["package"]<b["package"];});
    return result;
}
json State() { std::lock_guard lock(stateMutex); return state; }
json Action(const json& request) {
    if (!request.is_object() || !request.contains("action") || !request["action"].is_string())
        return {{"accepted",false},{"error","Expected an action string."}};
    const auto action=request["action"].get<std::string>();
    if (action=="clear_logs") {Update({{"logs",json::array()}});return {{"accepted",true}};}
    if(action=="refresh") return {{"accepted",true},{"processes",Processes()}};
    if(action!="probe" && action!="dump" && action!="library") return {{"accepted",false},{"error","Unknown action."}};
    if(request.contains("pid") && !request["pid"].is_number_integer()) return {{"accepted",false},{"error","PID must be an integer."}};
    std::lock_guard actionLock(actionMutex);
    const auto snapshot=State();
    if(snapshot["busy"].get<bool>()) return {{"accepted",false},{"error","An operation is already running."}};
    const int pid=request.value("pid",snapshot.value("pid",0));
    if(pid<=0 || Birth(pid).empty()) return {{"accepted",false},{"error","Select a running target process."}};
    if(action!="probe" && (!snapshot.value("probed",false) || snapshot.value("pid",0)!=pid))
        return {{"accepted",false},{"error","Probe the selected process first."}};
    if(worker.joinable()) worker.join();
    Update({{"busy",true},{"phase",action},{"error",""},{"objectsPercent",0},{"dumpPercent",0}});
    try { worker=std::thread([action,pid] {
        std::lock_guard operationLock(operationMutex);
        const auto started=Clock::now();
        std::string error;
        try {
            UEMemory::ReadSession readSession;
            if(action=="probe") Probe(pid);
            else if(action=="dump") Dump(pid);
            else Library(pid);
        } catch(const std::exception& e) {error=e.what();LOGE("%s",error.c_str());}
          catch(...) {error="Unexpected operation failure.";LOGE("%s",error.c_str());}
        if(action=="probe" && !error.empty()) {
            UEWrappers::Init(nullptr);
            session=Session{};
            Update({{"probed",false}});
        }
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
        LOGI("%s: %.2f ms",action.c_str(),elapsed);
        std::lock_guard lock(stateMutex);
        state["metrics"][action+"Ms"]=elapsed;
        const auto stats=UEMemory::GetReadStats();
        if (action!="library") {
            state["metrics"][action+"ReadCalls"]=stats.calls;
            state["metrics"][action+"ReadBytes"]=stats.bytes;
            state["metrics"][action+"CacheHits"]=stats.cacheHits;
            state["metrics"][action+"CacheMisses"]=stats.cacheMisses;
            state["metrics"][action+"ReadRequests"]=stats.requests;
            state["metrics"][action+"ShortReads"]=stats.shortReads;
        }
        state["error"]=error;
        state["generation"]=UEWrappers::GetInitGeneration();
        state["phase"]=error.empty()?"Complete":"Failed";
        state["busy"]=false;
    }); } catch (const std::exception& e) {
        Update({{"busy",false},{"phase","Failed"},{"error",e.what()}});
        return {{"accepted",false},{"error",e.what()}};
    }
    return {{"accepted",true}};
}
json Files() {
    const auto snapshot=State();
    const auto directory=snapshot.value("resultPath",std::string{});
    json files=json::array();
    if(directory.empty() || snapshot.value("busy",false)) return files;
    DIR* d=opendir(directory.c_str());
    if(!d) return files;
    while(auto* entry=readdir(d)) {
        const std::string name=entry->d_name;
        if(!SafeName(name) || name.ends_with(".partial")) continue;
        struct stat info{};
        if(lstat((directory+"/"+name).c_str(),&info) || !S_ISREG(info.st_mode)) continue;
        files.push_back({{"name",name},{"size",info.st_size},{"url","/api/file?name="+UrlEncode(name)}});
    }
    closedir(d);
    std::sort(files.begin(),files.end(),[](const json& a,const json& b){return a["name"]<b["name"];});
    return files;
}
std::string ResolveFile(const std::string& name) {
    if(!SafeName(name) || name.ends_with(".partial")) return {};
    const auto snapshot=State();
    if(snapshot.value("busy",false)) return {};
    const auto directory=snapshot.value("resultPath",std::string{});
    if(directory.empty()) return {};
    const auto path=directory+"/"+name;
    struct stat info{};
    return !lstat(path.c_str(),&info) && S_ISREG(info.st_mode)?path:std::string{};
}
void Wait() { std::lock_guard lock(actionMutex); if(worker.joinable()) worker.join(); }
void Shutdown() {
    Wait();
#ifdef UMT_NATIVE_UI
    SDKExplorer::Shutdown();
#endif
    Logger::SetSink(nullptr);
}
json Explore(const json& request) {
    std::unique_lock operationLock(operationMutex,std::try_to_lock);
    if(!operationLock.owns_lock()) return {{"error","A probe or dump is running."},{"busy",true}};
    int pid=0;
    {
        std::lock_guard lock(stateMutex);
        if(state.value("busy",false)) return {{"error","A probe or dump is running."},{"busy",true}};
        if(!state.value("probed",false)) return {{"error","Probe a process first."}};
        pid=state.value("pid",0);
    }
    try {
        CheckSession(pid);
        UEMemory::ReadSession readSession;
        return ExplorerApi::Read(request);
    } catch(const std::exception& e) {return {{"error",e.what()}};}
}
void RenderExplorer(bool world) {
#ifdef UMT_NATIVE_UI
    // Native rendering consumes JSON snapshots; background reads acquire the
    // same operation lock through App::Explore as Web/MCP. Never hold it here.
    SDKExplorer::Render(world);
#else
    (void)world;
#endif
}
} // namespace App
