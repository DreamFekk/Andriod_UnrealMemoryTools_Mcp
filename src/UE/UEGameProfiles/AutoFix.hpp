#pragma once

#include "../UEGameProfile.hpp"
using namespace UEMemory;

// Module-only version evidence (UE4/UE5) selects an initial layout. Names,
// object headers, and reflected fields are subsequently validated at runtime.
// Outline-number and case-preserving names are build options, not versions.
class AutoFixProfile : public IGameProfile
{
public:
    AutoFixProfile();
    explicit AutoFixProfile(const std::string &targetPackage);

    bool ArchSupprted() const override;
    std::string GetAppName() const override;
    std::vector<std::string> GetAppIDs() const override;

    bool isUsingCasePreservingName() const override { return GetOffsets()->Config.isUsingCasePreservingName; }
    bool IsUsingFNamePool() const override { return GetOffsets()->Config.IsUsingFNamePool; }
    bool isUsingOutlineNumberName() const override { return GetOffsets()->Config.isUsingOutlineNumberName; }

    UE_Offsets *GetOffsets() const override;

protected:
    uintptr_t GetGUObjectArrayPtr() const override;
    uintptr_t GetMatrix() const override;
    uintptr_t GetPhysx() const override;
    uintptr_t GetFrameCount() const override;
    uintptr_t GetNamesPtr() const override;

private:
    enum class UEFamily : uint8_t
    {
        Unknown = 0,
        UE4_00_17,
        UE4_18_19,
        UE4_20,
        UE4_21,
        UE4_22,
        UE4_23_24,
        UE4_25_27,
        UE5_00_02,
        UE5_03,
    };

    void DetectVersion() const;
    void EnsureOffsetsInited() const;
    UEFamily ScanVersionString() const;
    UEFamily GuessMajorFromSoName() const;

private:
    std::string _packageHint;

    mutable int _major=0, _minor=-1;
    mutable bool _versionDetected = false;
    mutable UEFamily _family = UEFamily::Unknown;
    mutable std::string _versionStr;

    mutable bool _useFNamePool = true;
    mutable bool _casePreserving = false;
    mutable bool _outlineNumber = false;

    mutable UE_Offsets _offsets;
    mutable bool _offsetsInited = false;
};
