#include "UEGameProfile.hpp"
#include "UERuntimeSupport.hpp"
#include <unordered_set>

using namespace UEMemory;

uintptr_t IGameProfile::ResolveLegacyTable(uintptr_t candidate) const {
    const UERuntime::Reader read=[](uintptr_t at,void* out,size_t n){return vm_rpm_ptr((void*)at,out,n);};
    std::vector<uintptr_t> candidates{candidate};
    // Both inline chunk tables and wrappers containing a chunk-table pointer occur.
    // Resolve at most three indirections, with no package-name special case.
    for(size_t i=0;i<candidates.size() && i<12;++i) {
        const auto base=candidates[i];
        if(!UERuntime::Pointer(base) || !kPtrValidator.isPtrReadable(base)) continue;
        for(uintptr_t header=0;header<=0x10;header+=sizeof(uintptr_t)) {
            const auto table=base+header;
            const auto chunk=UERuntime::Read<uintptr_t>(read,table);
            if(!UERuntime::Pointer(chunk) || !kPtrValidator.isPtrReadable(chunk)) continue;
            const auto entry=UERuntime::Read<uintptr_t>(read,chunk);
            if(!UERuntime::Pointer(entry) || !kPtrValidator.isPtrReadable(entry)) continue;
            for(uintptr_t indexOffset : {uintptr_t(0),uintptr_t(4),uintptr_t(8)}) {
                uint32_t index=0;
                if(!read(entry+indexOffset,&index,4) || (index>>1)!=0) continue;
                // Reject random zero words with one bounded read, before trying
                // terminated strings or following any further pointers.
                if(index&1) {
                    char16_t text[5]{};
                    if(!read(entry+indexOffset+4,text,sizeof(text)) || text[4]) continue;
                    DecryptNameWide(text,4);
                    if(std::u16string_view(text,4)!=u"None") continue;
                } else {
                    char text[5]{};
                    if(!read(entry+indexOffset+4,text,sizeof(text)) || text[4]) continue;
                    DecryptNameAnsi(text,4);
                    if(std::string_view(text,4)!="None") continue;
                }
                UE_Offsets layout=*GetOffsets();
                layout.Config.IsUsingFNamePool=false;
                layout.FNameEntry.Index=indexOffset;layout.FNameEntry.Name=indexOffset+4;
                layout.FNameEntry.GetIsWide=[](uint32_t n){return (n&1)!=0;};
                auto name=[&](uintptr_t p){return UERuntime::Name(read,p,layout,{},
                    [this](char* s,size_t n){DecryptNameAnsi(s,n);},
                    [this](char16_t* s,size_t n){DecryptNameWide(s,n);});};
                if(name(entry)!="None") continue;
                bool anchor=false;
                for(size_t slot=1;slot<8;++slot) {
                    const auto item=UERuntime::Read<uintptr_t>(read,chunk+slot*sizeof(uintptr_t));
                    if(!UERuntime::Pointer(item)) continue;
                    const auto text=name(item);
                    if(text=="ByteProperty" || text=="IntProperty" || text=="BoolProperty") {anchor=true;break;}
                }
                if(!anchor) continue;
                GetOffsets()->FNameEntry=layout.FNameEntry;
                GetOffsets()->Config.IsUsingFNamePool=false;
                return table;
            }
            if(candidates.size()<12 && std::find(candidates.begin(),candidates.end(),chunk)==candidates.end()) candidates.push_back(chunk);
        }
    }
    return 0;
}

bool IGameProfile::DetectNamePoolBlock(uintptr_t block) const {
    if(!UERuntime::Pointer(block)) return false;
    const UERuntime::Reader read=[](uintptr_t at,void* out,size_t n){return vm_rpm_ptr((void*)at,out,n);};
    // Verify two successive entries. ByteProperty's position depends on stride,
    // case-preserving headers and UTF-16; it is not always block + 8.
    for(const auto header : {uintptr_t(0),uintptr_t(4)}) for(const unsigned shift : {6u,1u}) {
        uint16_t value=0;
        if(!read(block+header,&value,2) || (value>>shift)!=4) continue;
        UE_Offsets layout=*GetOffsets();
        layout.Config.IsUsingFNamePool=true;
        layout.FNamePoolEntry.Header=header;
        layout.FNamePoolEntry.GetLength=[shift](uint16_t h)->size_t{return h>>shift;};
        layout.FNamePoolEntry.GetIsWide=[](uint16_t h){return (h&1)!=0;};
        for(const uintptr_t stride : {uintptr_t(2),uintptr_t(4)}) {
            if(header==4 && stride!=4) continue;
            const auto name=[&](uintptr_t p){return UERuntime::Name(read,p,layout,{},
                [this](char* s,size_t n){DecryptNameAnsi(s,n);},
                [this](char16_t* s,size_t n){DecryptNameWide(s,n);});};
            if(name(block)!="None") continue;
            const uintptr_t bytes=header+2+4*((value&1)?2:1);
            const uintptr_t next=(bytes+stride-1)&~(stride-1);
            if(name(block+next)!="ByteProperty") continue;
            auto* o=GetOffsets();
            o->Config.IsUsingFNamePool=true;
            o->Config.isUsingCasePreservingName=header==4;
            o->FNamePool.Stride=stride;o->FNamePoolEntry=layout.FNamePoolEntry;
            o->FName.Size=UE_DefaultOffsets::kGetFNameSize(header==4,o->Config.isUsingOutlineNumberName);
            o->FName.DisplayIndex=header==4?4:0;
            o->FName.Number=o->Config.isUsingOutlineNumberName?0:(header==4?8:4);
            LOGI("[Names] Validated pool entries: header=0x%lx stride=%lu lengthShift=%u",header,stride,shift);
            return true;
        }
    }
    return false;
}
