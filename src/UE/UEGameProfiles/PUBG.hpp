#pragma once

#include "../UEGameProfile.hpp"
#include "../UERuntimeSupport.hpp"
#include <unordered_set>
using namespace UEMemory;

class PUBGProfile : public IGameProfile
{
public:
    PUBGProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "PUBG";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {
            "com.tencent.ig",
            "com.rekoo.pubgm",
            "com.pubg.imobile",
            "com.pubg.krmobile",
            "com.vng.pubgmobile",
        };
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return false;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

    uintptr_t GetGUObjectArrayPtr() const override
    {
        if(auto generic=IGameProfile::GetGUObjectArrayPtr()) return generic;
        const auto map=isEmulator()?PATTERN_MAP_TYPE::ANY_R:PATTERN_MAP_TYPE::ANY_X;
        return Arm64::Decode_ADRP_ADD(findIdaPattern(map,"12 40 B9 ? 3E 40 B9 ? ? ? 6B ? ? ? 54 ? ? ? ? ? ? ? 91",0xf));
    }

    uintptr_t GetNamesPtr() const override
    {
        // Shared/PUBG.h: global -> manager -> manager+0x110 -> name array.
        const auto elf=GetUnrealELF();
        std::unordered_set<uintptr_t> seen;
        std::vector<uint8_t> buffer(0x40000);
        for(const auto& segment:elf.segments()) {
            if(!segment.readable || !segment.writeable || segment.executable) continue;
            for(size_t offset=0;offset<segment.length;offset+=buffer.size()) {
                const auto n=std::min(buffer.size(),segment.length-offset);
                if(!vm_rpm_ptr((void*)(segment.startAddress+offset),buffer.data(),n)) continue;
                for(size_t i=0;i+sizeof(uintptr_t)<=n;i+=sizeof(uintptr_t)) {
                    uintptr_t manager=0;std::memcpy(&manager,buffer.data()+i,sizeof(manager));
                    if(!UERuntime::Pointer(manager) || !kPtrValidator.isPtrReadable(manager) || !seen.insert(manager).second) continue;
                    const auto array=vm_rpm_ptr<uintptr_t>((void*)(manager+(sizeof(uintptr_t)==8?0x110:0x88)));
                    if(!UERuntime::Pointer(array)) continue;
                    if(auto table=ResolveLegacyTable(array)) {
                        _resolvedLegacyTable=table;
                        LOGI("[PUBG] Validated manager name array: slot=0x%lx table=0x%lx",segment.startAddress+offset+i,table);
                        return segment.startAddress+offset+i;
                    }
                }
            }
        }
        const auto map=isEmulator()?PATTERN_MAP_TYPE::ANY_R:PATTERN_MAP_TYPE::ANY_X;
        const auto descriptor=Arm64::Decode_ADRP_ADD(findIdaPattern(map,"81 80 52 ? ? ? ? ? 81 80 52 ? 03 1F 2A",0x1f));
        if(descriptor) {
            auto array=UERuntime::PubgChain([](uintptr_t at,void* out,size_t n){return vm_rpm_ptr((void*)at,out,n);},descriptor);
            if(auto table=ResolveLegacyTable(array)) {_resolvedLegacyTable=table;return descriptor;}
        }
        return IGameProfile::GetNamesPtr();
    }
    uintptr_t GetFrameCount() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;


        std::string ida_pattern = "? ? ? ? ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 F3 03 01 2A";
        const int step = 0;
        auto FrameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
        if (FrameOff != 0)
            return vm_rpm_ptr<uintptr_t>((void*)FrameOff);
        return 0;
    }

    uintptr_t GetMatrix() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x34},
            {"68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x3c},
            {"? ? ? 54 ? ? ? 14 ? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x2c},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }
        return 0;
    }

    uintptr_t GetPhysx() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? B0 00 03 3F D6", 0x0},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step),0x20);
            //printf("%lx\n", vm_rpm_ptr<uintptr_t>((void*)adrl));
            if (adrl != 0) return adrl;
        }
        return 0;
    }
    UE_Offsets *GetOffsets() const override
    {
        return &_offsets;
    }
private:
    mutable UE_Offsets _offsets=[] {
        auto o=UE_DefaultOffsets::UE4_18_19(false);
        o.FNameEntry.Index=sizeof(void*);o.FNameEntry.Name=sizeof(void*)+4;
        return o;
    }();
};
