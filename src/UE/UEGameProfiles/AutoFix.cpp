#include "AutoFix.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include "../../AutoFix/Arm64Decode.hpp"
#include "../UEMemory.hpp"
#include "../UERuntimeSupport.hpp"
#include "../UEWrappers.hpp"

using namespace UEMemory;

AutoFixProfile::AutoFixProfile() = default;

AutoFixProfile::AutoFixProfile(const std::string &targetPackage)
    : _packageHint(targetPackage)
{
}

bool AutoFixProfile::ArchSupprted() const
{
    auto e_machine = GetUnrealELF().header().e_machine;
    return e_machine == EM_AARCH64;
}

std::string AutoFixProfile::GetAppName() const
{
    return "Auto (UE4/UE5 Generic)";
}

std::vector<std::string> AutoFixProfile::GetAppIDs() const
{
    if (!_packageHint.empty())
        return {_packageHint};
    return {"<auto>"};
}

AutoFixProfile::UEFamily AutoFixProfile::ScanVersionString() const
{
    constexpr size_t chunkSize=1024*1024, overlap=96;
    auto scan=[&](uintptr_t start,size_t size) {
        std::vector<char> buffer(chunkSize+overlap);
        for(size_t at=0;at<size;at+=chunkSize) {
            const auto n=std::min(buffer.size(),size-at);
            if(!vm_rpm_ptr((void*)(start+at),buffer.data(),n)) continue;
            const auto version=UERuntime::ParseVersion(std::string_view(buffer.data(),n));
            if(!version) continue;
            _versionStr=std::to_string(version.major)+"."+std::to_string(version.minor);
            _major=version.major; _minor=version.minor;
            if(version.major==5) return version.minor>=2?UEFamily::UE5_03:UEFamily::UE5_00_02;
            if(version.minor<=17) return UEFamily::UE4_00_17;
            if(version.minor<=19) return UEFamily::UE4_18_19;
            if(version.minor==20) return UEFamily::UE4_20;
            if(version.minor==21) return UEFamily::UE4_21;
            if(version.minor==22) return UEFamily::UE4_22;
            if(version.minor<=24) return UEFamily::UE4_23_24;
            return UEFamily::UE4_25_27;
        }
        return UEFamily::Unknown;
    };
    const auto elf=GetUnrealELF();
    // Prefer non-executable read-only module data. No whole-process heap scan.
    for(int pass=0;pass<2;++pass) for(const auto& segment:elf.segments()) {
        if(!segment.readable || (pass==0)!=( !segment.executable && !segment.writeable)) continue;
        const auto found=scan(segment.startAddress,segment.length);
        if(found!=UEFamily::Unknown) return found;
    }
    return UEFamily::Unknown;
}

AutoFixProfile::UEFamily AutoFixProfile::GuessMajorFromSoName() const
{
    // Epic 在 UE5 起把动态库统一改名为 libUnreal.so，UE4 时代是 libUE4.so。
    // 直接据此映射到一个保守的 fallback 大版本即可；具体小版本由 ScanVersionString
    // 的精确路径优先决定。
    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid()) return UEFamily::Unknown;

    const std::string elfPath = ue_elf.filePath();
    if (elfPath.find("libUnreal.so") != std::string::npos)
        return UEFamily::UE5_03;
    if (elfPath.find("libUE4.so") != std::string::npos)
        return UEFamily::UE4_25_27;
    return UEFamily::Unknown;
}

void AutoFixProfile::DetectVersion() const
{
    if(_versionDetected) return;
    _versionDetected=true;
    _family=ScanVersionString();
    if(_family==UEFamily::Unknown) {
        _family=GuessMajorFromSoName();
        if(_family==UEFamily::Unknown) _family=UEFamily::UE4_25_27;
        _major=_family==UEFamily::UE5_03?5:4;
        _minor=-1;
        _versionStr=_major==5?"UE5 (library hint; minor unknown)":"UE4 (fallback; minor unknown)";
    }
    _useFNamePool=_family>=UEFamily::UE4_23_24;
    _outlineNumber=false; // Build option, never implied by a version or .so filename.
    _casePreserving=false;
    LOGI("[AutoFix] Engine %s; FName configuration will be validated from memory",_versionStr.c_str());
}

void AutoFixProfile::EnsureOffsetsInited() const
{
    if (_offsetsInited) return;
    _offsetsInited = true;
    DetectVersion();

    switch (_family)
    {
    case UEFamily::UE4_00_17:
        _offsets = UE_DefaultOffsets::UE4_00_17(_casePreserving); break;
    case UEFamily::UE4_18_19:
        _offsets = UE_DefaultOffsets::UE4_18_19(_casePreserving); break;
    case UEFamily::UE4_20:
        _offsets = UE_DefaultOffsets::UE4_20(_casePreserving); break;
    case UEFamily::UE4_21:
        _offsets = UE_DefaultOffsets::UE4_21(_casePreserving); break;
    case UEFamily::UE4_22:
        _offsets = UE_DefaultOffsets::UE4_22(_casePreserving); break;
    case UEFamily::UE4_23_24:
        _offsets = UE_DefaultOffsets::UE4_23_24(_casePreserving); break;
    case UEFamily::UE4_25_27:
        _offsets = UE_DefaultOffsets::UE4_25_27(_casePreserving); break;
    case UEFamily::UE5_00_02:
        _offsets = UE_DefaultOffsets::UE5_00_02(_casePreserving, _outlineNumber); break;
    case UEFamily::UE5_03:
        _offsets = UE_DefaultOffsets::UE5_03(_casePreserving, _outlineNumber); break;
    default:
        _offsets = UE_DefaultOffsets::UE4_25_27(_casePreserving); break;
    }
    _offsets.Config.structAlignmentIs16Bit=(_major==5 && _minor>=6);
}

UE_Offsets *AutoFixProfile::GetOffsets() const
{
    EnsureOffsetsInited();
    return &_offsets;
}

uintptr_t AutoFixProfile::GetGUObjectArrayPtr() const
{
    return IGameProfile::GetGUObjectArrayPtr();
}

uintptr_t AutoFixProfile::GetFrameCount() const
{
    struct Pattern
    {
        const char *ida;
        int step;
    };

    static const Pattern kPatterns[] = {
        {"C0 03 5F D6 ? ? ? ? 08 ? ? ? 00 01 40 F9 C0 03 5F D6 FD 7B BF A9", 4},
        {"? ? ? F0 ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 FD 03 00 91 ? ? ? D0", 0},
        {"? ? ? F0 ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 F3 03 01 2A", 0},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t frameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step));
        if (frameOff != 0) return vm_rpm_ptr<uintptr_t>((void *)frameOff);
    }

    return 0;
}

uintptr_t AutoFixProfile::GetMatrix() const
{
    struct Pattern
    {
        const char *ida;
        int step;
    };

    static const Pattern kPatterns[] = {
        {"08 3D 40 F9 00 01 3F D6 E8 03 13 AA ? ? ? F9", 0x18},
        {"FD ? ? A9 28 ? ? F9 F3 ? ? F8 C0 03 5F D6", -0x1c},
        {"00 01 3F D6 E8 03 13 AA 60 ? 00 F9 ? ? ? A9", 14},
        {"? ? ? D0 ? ? ? F9 ? ? ? 32 6B 7D 40 93 6C 25 0A 9B ? ? ? F9 BF 01 08 EB ? ? ? 54 6B 25 0A 9B ? ? ? B9 ? ? ? 31 ? ? ? 54 ? ? ? 14 ? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14", 0},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t addr = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step));
        if (addr != 0) return addr;
    }

    return 0;
}

uintptr_t AutoFixProfile::GetPhysx() const
{
    struct Pattern
    {
        const char *ida;
        int step;
        int decodeStep;
        bool dereference;
    };

    static const Pattern kPatterns[] = {
        {"E1 ? ? ? 40 00 40 BD F4 03 02 AA", 0x28, 0x8, true},
        {"48 ? ? ? F3 03 04 AA F5 03 03 2A", -0xc, 0x8, true},
        {"? ? ? B0 00 03 3F D6", 0x0, 0x20, false},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t addr = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step), pattern.decodeStep);
        if (addr == 0) continue;
        return pattern.dereference ? vm_rpm_ptr<uintptr_t>((void *)addr) : addr;
    }

    return 0;
}

uintptr_t AutoFixProfile::GetNamesPtr() const
{
    return IGameProfile::GetNamesPtr();
}
