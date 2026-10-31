#pragma once

#include "../UEGameProfile.hpp"
using namespace UEMemory;

class PUBGMHDProfile : public IGameProfile
{
public:
    PUBGMHDProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "和平精英";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.pubgmhd"};
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
        return IGameProfile::GetGUObjectArrayPtr();
    }
    void print_func() const
    {
        auto base = GetUEVars()->GetBaseAddress();
        //printf("0x%lx\n", base);
        std::string CalShootTargetLocationBP = " A2 40 20 1E 81 40 20 1E 60 40 20 1E C0 03 5F D6 ? ? ? F9";
        std::string DecArray = " ? ? ? 90 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? B9 ? ? ? A9";
        const int step = 0;
        std::string GetAccessoriesAllRecoilFactorModifier = "40 08 20 1E 28 00 38 1E";
        std::string AnimationKick = "0B 08 2B 1E 00 01 3F D6";
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        printf("CalShootTargetLocationBPOffset:0x%lX\n", findIdaPattern(map_type, CalShootTargetLocationBP, 0) - base);
        printf("DecArrayOffset:0x%lX\n", Arm64::Decode_ADRP_ADD(findIdaPattern(map_type, DecArray, step)) - base);
        printf("GetAccessoriesAllRecoilFactorModifierOffset:0x%lX\n", findIdaPattern(map_type, GetAccessoriesAllRecoilFactorModifier, 0) - base);
        printf("AnimationKickOffset:0x%lX\n", findIdaPattern(map_type, AnimationKick, 0) - base);
    }
    uintptr_t GetNamesPtr() const override
    {
        print_func();
        return IGameProfile::GetNamesPtr();
    }
    uintptr_t GetFrameCount() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;


        std::string ida_pattern = "? ? ? F0 ? ? ? F9 ? ? ? F1 ? ? ? 9A ? ? ? F9 ? ? ? F9 C0 03 5F D6";
        const int step = 0;
        auto FrameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
        if (FrameOff != 0)
            return FrameOff;
        return 0;
    }

    uintptr_t GetMatrix() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? ? ? ? ? 91 ? ? ? ? ? ? ? 91 ? ? ? ? ? ? ? 91 ? ? ? A9 00 04 18 6E ? ? ? F9 ? ? ? FD ? ? ? B9 ? ? ? 3D ? ? ? 95 ? ? ? ? ? ? ? 91 ? ? ? 95 ? ? ? F9 ? ? ? 17", 0x0},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }
        return 0;
    }

    uintptr_t GetPhysx() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? 90 ? ? ? 94 ? ? ? F9 ? ? ? B4 ? ? ? F9", 0x0},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step),0x8);
            //printf("%lx\n", vm_rpm_ptr<uintptr_t>((void*)adrl));
            if (adrl != 0) return adrl;
        }
        return 0;
    }
    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_18_19(isUsingCasePreservingName());

        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.FNameEntry.Index = sizeof(void *);
            offsets.FNameEntry.Name = sizeof(void *) + sizeof(int32_t);
            offsets.TUObjectArray.NumElements = 0x38;
        }

        return &offsets;
    }
};