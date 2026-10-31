#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../Utils/Logger.hpp"

#include "UEMemory.hpp"
#include "UEOffsets.hpp"

enum class PATTERN_MAP_TYPE : int8_t
{
    ANY_R,  // Search in any private readable map

    ANY_X,  // Search in any private readable & executable map

    ANY_W,  // Search in any private readable & writeable map

    BSS,  // Search in .bss maps
};

class IGameProfile
{
public:
protected:
    UEVars _UEVars;
    mutable ElfScanner _cachedELF;
    mutable pid_t _cachedPID = 0;
    mutable uintptr_t _cachedNames = 0, _cachedNamesPtr = 0;
    mutable uintptr_t _resolvedLegacyTable = 0;

public:
    virtual ~IGameProfile() = default;

    UEVarsInitStatus InitUEVars();
    const UEVars *GetUEVars() const { return &_UEVars; }

    virtual ElfScanner GetUnrealELF() const;

    // arch support check
    virtual bool ArchSupprted() const = 0;

    virtual std::string GetAppName() const = 0;

    virtual std::vector<std::string> GetAppIDs() const = 0;

    virtual bool isUsingCasePreservingName() const = 0;

    virtual bool IsUsingFNamePool() const = 0;

    virtual bool isUsingOutlineNumberName() const = 0;

    virtual UE_Offsets *GetOffsets() const = 0;
    virtual int32_t ReadObjectIndex(uintptr_t address) const;
    virtual uintptr_t ReadObjectClass(uintptr_t address) const;
    virtual uintptr_t ReadObjectOuter(uintptr_t address) const;
    virtual bool RequireValidatedLayout() const { return false; }

protected:
    virtual uintptr_t GetGUObjectArrayPtr() const;
    virtual uintptr_t GetMatrix()  const = 0;
    virtual uintptr_t GetPhysx() const = 0;
    virtual uintptr_t GetFrameCount() const = 0;
    virtual uintptr_t GetStaticFindObject() const;
    virtual uintptr_t GetNativeAndroidApp() const;
    virtual uintptr_t GetProcessEvent() const;
    // GNames / NamePoolData
    virtual uintptr_t GetNamesPtr() const;
    virtual void DecryptNameAnsi(char*, size_t) const {}
    virtual void DecryptNameWide(char16_t*, size_t) const {}
    uintptr_t ResolveLegacyTable(uintptr_t candidate) const;
    bool DetectNamePoolBlock(uintptr_t block) const;

    virtual uint8_t *GetNameEntry(int32_t id) const;
    // can override if decryption is needed
    virtual std::string GetNameEntryString(uint8_t *entry) const;
    virtual std::string GetNameByID(int32_t id) const;

    virtual bool isEmulator() const;

    virtual uintptr_t findIdaPattern(PATTERN_MAP_TYPE map_type,
                                     const std::string &pattern, const int step,
                                     uint32_t skip_result = 0) const;


};
