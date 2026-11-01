#include "UEGameProfile.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <vector>
#include <unordered_set>

#include "UEMemory.hpp"
#include "UEWrappers.hpp"
#include "UERuntimeSupport.hpp"
#include "ObjectArrayLayout.hpp"

using namespace UEMemory;

namespace
{
    constexpr uintptr_t kArm64PageSize = 0x1000;
    constexpr uintptr_t kMinReadablePtr = 0x10000;

    bool IsLikelyReadablePtr(uintptr_t value)
    {
        return value >= kMinReadablePtr && kPtrValidator.isPtrReadable(value);
    }

    bool IsLikelyObjectName(const std::string &name)
    {
        if (name.empty() || name.size() > 96)
            return false;

        for (char c : name)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (!(std::isalnum(uc) || c == '_' || c == '/' || c == '.'))
                return false;
        }

        return true;
    }


    bool HasAppId(const IGameProfile *profile, const char *appId)
    {
        if (!profile || !appId || !*appId)
            return false;
        for (const auto &id : profile->GetAppIDs())
        {
            if (id == appId)
                return true;
        }
        return false;
    }

    uintptr_t GetModuleReadableEnd(const ElfScanner &ue_elf)
    {
        uintptr_t end = ue_elf.base();
        for (const auto &seg : ue_elf.segments())
        {
            if (seg.readable)
                end = (end > static_cast<uintptr_t>(seg.endAddress))
                          ? end
                          : static_cast<uintptr_t>(seg.endAddress);
        }
        return end;
    }

    uintptr_t BruteForceDeltaForceNames(const IGameProfile *profile, const ElfScanner &ue_elf, const UE_Offsets *off)
    {
        if (!profile || !off || !ue_elf.isValid())
            return 0;

        // 已知的 Delta Force CN FName 解密参数（与 dump-7 保持一致）
        constexpr uintptr_t kFNameStride = 0x2;
        constexpr uintptr_t kFNameEntryToString = 0x2;
        constexpr uintptr_t kGNamesToFNamePool = 0x38;
        constexpr uintptr_t kFNamePoolToBlocks = 0x0;
        constexpr uintptr_t kSearchStartOff = 0x18000000;
        constexpr uintptr_t kSearchEndOff = 0x1E000000;
        constexpr uintptr_t kMinGamePtr = 0x4000000000ULL;
        constexpr uintptr_t kMaxGamePtr = 0x8000000000ULL;

        static const int kProbeIds[] = {0, 1, 2, 5, 10, 20, 50, 100};
        static const char *kKnownNames[] = {
            "None", "ByteProperty", "IntProperty", "BoolProperty",
            "FloatProperty", "ObjectProperty", "NameProperty",
            "StructProperty", "ArrayProperty", "Object", "Class", "Function"
        };

        const uintptr_t searchStart = ue_elf.base() + kSearchStartOff;
        const uintptr_t searchEnd = ue_elf.base() + kSearchEndOff;
        if (searchEnd <= searchStart)
            return 0;

        const size_t totalRange = (searchEnd - searchStart) / kArm64PageSize;
        size_t checked = 0;
        LOGI("[Bootstrap] DeltaForce GNames brute force start: base=0x%lx range=[0x%lx, 0x%lx) pages=%zu",
             static_cast<unsigned long>(ue_elf.base()),
             static_cast<unsigned long>(searchStart),
             static_cast<unsigned long>(searchEnd),
             totalRange);

        for (uintptr_t addr = searchStart; addr < searchEnd; addr += kArm64PageSize)
        {
            ++checked;
            const uintptr_t candidate = addr;
            if ((checked % 5000) == 0)
            {
                const size_t percent = totalRange ? ((checked * 100) / totalRange) : 0;
                LOGD("[Bootstrap] DeltaForce GNames progress: checked=%zu/%zu (%zu%%) current=0x%lx",
                     checked, totalRange, percent, static_cast<unsigned long>(candidate));
            }

            int validCount = 0;
            for (int testIdx : kProbeIds)
            {
                const uint32_t block = static_cast<uint32_t>(testIdx >> 18);
                const uint32_t offset = static_cast<uint32_t>(testIdx & 0x3FFFF);
                const uintptr_t fNamePool = candidate + kGNamesToFNamePool;
                const uintptr_t namePoolChunk = vm_rpm_ptr<uintptr_t>(
                    (void *)(fNamePool + kFNamePoolToBlocks + block * sizeof(uintptr_t)));

                if (namePoolChunk < kMinGamePtr || namePoolChunk > kMaxGamePtr)
                    continue;

                const uintptr_t fNameEntry = namePoolChunk + kFNameStride * offset;
                const uint16_t fNameEntryHeader = vm_rpm_ptr<uint16_t>((void *)fNameEntry);
                const bool isWide = (fNameEntryHeader & 1) != 0;
                const int strLength = (fNameEntryHeader >> 6) & 0x3FF;

                if (strLength > 0 && strLength < 100 && !isWide)
                {
                    const uintptr_t strPtr = fNameEntry + kFNameEntryToString;
                    uint8_t key = 0;
                    switch (strLength % 9)
                    {
                    case 0:
                        key = ((strLength & 0x1F) + strLength + 0x80) | 0x7F;
                        break;
                    case 1:
                        key = ((strLength ^ 0xDF) + strLength + 0x80) | 0x7F;
                        break;
                    case 2:
                        key = ((strLength | 0xCF) + strLength + 128) | 0x7F;
                        break;
                    case 3:
                        key = (33 * strLength + 128) | 0x7F;
                        break;
                    case 4:
                        key = (strLength + (strLength >> 2) + 0x80) | 0x7F;
                        break;
                    case 5:
                        key = (3 * strLength + 133) | 0x7F;
                        break;
                    case 6:
                        key = (((4 * strLength) | 5) + strLength + 128) | 0x7F;
                        break;
                    case 7:
                        key = (((strLength >> 4) | 7) + strLength + 128) | 0x7F;
                        break;
                    case 8:
                        key = ((strLength ^ 0xC) + strLength + 0x80) | 0x7F;
                        break;
                    default:
                        key = ((strLength ^ 0x40) + strLength + 128) | 0x7F;
                        break;
                    }

                    char encNameBuf[100] = {};
                    if (vm_rpm_ptr((void *)strPtr, encNameBuf, static_cast<size_t>(strLength)))
                    {
                        std::string decrypted(static_cast<size_t>(strLength), '\0');
                        for (int i = 0; i < strLength; ++i)
                            decrypted[static_cast<size_t>(i)] =
                                static_cast<char>(static_cast<uint8_t>(encNameBuf[i]) ^ key);

                        for (const char *known : kKnownNames)
                        {
                            if (decrypted == known)
                            {
                                LOGI("[Bootstrap] DeltaForce candidate hit: candidate=0x%lx testIdx=%d block=%u offset=0x%x len=%d key=0x%x text='%s'",
                                     static_cast<unsigned long>(candidate),
                                     testIdx,
                                     block,
                                     offset,
                                     strLength,
                                     static_cast<unsigned int>(key),
                                     decrypted.c_str());
                                ++validCount;
                                break;
                            }
                        }

                        if (validCount >= 1)
                        {
                            LOGI("[Bootstrap] DeltaForce GNames @ 0x%lx (validated with %d known names, checked=%zu/%zu)",
                                 static_cast<unsigned long>(candidate), validCount, checked, totalRange);
                            return candidate;
                        }
                    }
                }
            }
        }

        LOGW("[Bootstrap] DeltaForce GNames brute force failed after checking %zu/%zu pages",
             checked, totalRange);
        return 0;
    }
    std::vector<uintptr_t> MakeCandidateList(uintptr_t primary, std::initializer_list<uintptr_t> fallbacks)
    {
        std::vector<uintptr_t> result;
        result.reserve(fallbacks.size() + 1);

        auto addUnique = [&result](uintptr_t value)
        {
            if (std::find(result.begin(), result.end(), value) == result.end())
                result.push_back(value);
        };

        addUnique(primary);
        for (uintptr_t value : fallbacks)
            addUnique(value);

        return result;
    }

    uintptr_t ObjectArrayPrefix(const UERuntime::ObjectArrayLayout& layout) {
        // The validator uses the pointer slot as its origin. Some custom
        // containers store NumElements before that pointer; exported offsets
        // remain relative to an aligned container and must stay nonnegative.
        if(layout.countOffset>=0)return 0;
        return (uintptr_t(-int64_t(layout.countOffset))+sizeof(uintptr_t)) & ~(sizeof(uintptr_t)-1);
    }

    int32_t RelativeObjectCount(const UE_Offsets* off) {
        return int32_t(int64_t(off->TUObjectArray.NumElements)-int64_t(off->TUObjectArray.Objects));
    }

    void ApplyObjectArrayLayout(UE_Offsets* off,const UERuntime::ObjectArrayLayout& layout,uintptr_t guObject) {
        const auto prefix=ObjectArrayPrefix(layout);
        off->FUObjectArray.ObjObjects=layout.slot-prefix-guObject;
        off->TUObjectArray.Objects=prefix;
        off->TUObjectArray.NumElements=uintptr_t(int64_t(prefix)+layout.countOffset);
        off->TUObjectArray.NumElementsPerChunk=layout.chunkSize;
        off->FUObjectItem.Size=layout.itemSize;
        off->FUObjectItem.Object=layout.itemObject;
        off->UObject.InternalIndex=layout.indexOffset;
        LOGI("[Bootstrap] Validated object table: slot=0x%lx count=%u countOff=%d stride=0x%x item=0x%x index=0x%x chunk=%u score=%d",
            layout.slot,layout.count,layout.countOffset,layout.itemSize,layout.itemObject,layout.indexOffset,layout.chunkSize,layout.score);
    }

    bool BootstrapCoreObjectArrayOffsets(const IGameProfile* profile,UE_Offsets* off,uintptr_t guObject) {
        if(!profile || !off || !IsLikelyReadablePtr(guObject)) return false;
        const UERuntime::Reader read=[](uintptr_t at,void* out,size_t n){return vm_rpm_ptr((void*)at,out,n);};
        const auto readable=[](uintptr_t p){return kPtrValidator.isPtrReadable(p);};
        const auto index=[profile](uintptr_t at){return profile->ReadObjectIndex(at);};
        UERuntime::ObjectArrayLayout best;
        auto offsets=MakeCandidateList(off->FUObjectArray.ObjObjects+off->TUObjectArray.Objects,{0,8,16,24,32,40,48,56,64});
        for(auto offset:offsets) {
            auto layout=UERuntime::ValidateObjectArray(read,guObject+offset,readable,index,uint32_t(off->FUObjectItem.Size),RelativeObjectCount(off),uint32_t(off->UObject.InternalIndex));
            if(layout.slot<guObject+ObjectArrayPrefix(layout))continue;
            if(layout.score>best.score) best=layout;
            if(layout.score>=24) break;
        }
        if(!best) {LOGE("[Bootstrap] Object table rejected: index-to-slot identity could not be validated");return false;}
        ApplyObjectArrayLayout(off,best,guObject);return true;
    }

    uint64_t DecodeADRP(uint64_t pc, uint32_t insn)
    {
        uint64_t immhi = (insn >> 5) & 0x7FFFF;
        uint64_t immlo = (insn >> 29) & 0x3;
        uint64_t imm = (immhi << 2) | immlo;
        if (imm & (1ULL << 20))
            imm |= ~((1ULL << 21) - 1);

        int64_t offset = static_cast<int64_t>(imm) << 12;
        uint64_t base = pc & ~0xFFFULL;
        return base + offset;
    }

    uint64_t DecodeADD(uint32_t insn)
    {
        uint32_t imm12 = (insn >> 10) & 0xFFF;
        uint32_t shift = (insn >> 22) & 0x1;
        return static_cast<uint64_t>(imm12) << (shift ? 12 : 0);
    }

    bool IsADD(uint32_t insn)
    {
        return (insn & 0x7F800000) == 0x11000000;
    }

    bool IsSubSP(uint32_t insn)
    {
        return (insn & 0xFFC00000) == 0xD1000000 &&
               ((insn >> 5) & 0x1F) == 31 &&
               (insn & 0x1F) == 31;
    }

    bool IsStpFpLr(uint32_t insn)
    {
        return (((insn & 0xFFC00000) == 0xA9000000) ||
                ((insn & 0xFFC00000) == 0xA9800000)) &&
               ((insn >> 5) & 0x1F) == 31;
    }

    bool IsStrInstruction(uint32_t insn)
    {
        return ((insn & 0xFFC00000) == 0xF8000000) ||
               ((insn & 0xFFC00000) == 0xFC000000) ||
               ((insn & 0xFFC00000) == 0xF9000000);
    }

    uintptr_t FixTaggedPtr(uintptr_t value)
    {
        if (value > 0xB400000000000000ULL && value < 0xB400008000000000ULL)
            return value & 0x7FFFFFFFFFULL;
        return value;
    }

    uintptr_t FindFunctionStart(uintptr_t addr, uintptr_t search_start)
    {
        if (!addr) return 0;
        if (search_start > addr) search_start = 0;

        for (uintptr_t pc = addr; ; pc -= 4)
        {
            uint32_t insn = vm_rpm_ptr<uint32_t>((void *)pc);
            if (IsStpFpLr(insn) || IsSubSP(insn))
                return pc;

            if (pc <= search_start || pc < 4)
                break;
        }

        return 0;
    }

    uintptr_t FindWideDataInSegments(const ElfScanner &ue_elf, const void *data, size_t size)
    {
        for (const auto &seg : ue_elf.segments())
        {
            if (!seg.readable) continue;
            uintptr_t hit = kMgr.memScanner.findDataFirst(seg.startAddress, seg.endAddress, data, size);
            if (hit) return hit;
        }
        return 0;
    }

    std::vector<uintptr_t> FindADRPForTarget(const ElfScanner &ue_elf, uintptr_t target_addr)
    {
        std::vector<uintptr_t> result;
        const uint64_t target_page = target_addr & ~0xFFFULL;
        std::vector<uint8_t> buf(kArm64PageSize);

        for (const auto &seg : ue_elf.segments())
        {
            if (!seg.readable || !seg.executable) continue;

            for (uintptr_t page = seg.startAddress; page < seg.endAddress; page += kArm64PageSize)
            {
                size_t to_read = std::min<size_t>(kArm64PageSize, seg.endAddress - page);
                if (to_read < sizeof(uint32_t)) continue;
                if (!vm_rpm_ptr((void *)page, buf.data(), to_read))
                    continue;

                for (size_t i = 0; i + sizeof(uint32_t) <= to_read; i += sizeof(uint32_t))
                {
                    uint32_t insn = 0;
                    memcpy(&insn, buf.data() + i, sizeof(uint32_t));
                    if ((insn & 0x9F000000) != 0x90000000)
                        continue;

                    uintptr_t pc = page + i;
                    if (DecodeADRP(pc, insn) == target_page)
                        result.push_back(pc);
                }
            }
        }

        return result;
    }

    uintptr_t FilterADRPWithADD(const std::vector<uintptr_t> &adrp_candidates, uintptr_t target_addr)
    {
        for (uintptr_t adrp_addr : adrp_candidates)
        {
            uint32_t adrp_insn = vm_rpm_ptr<uint32_t>((void *)adrp_addr);
            if (!adrp_insn) continue;

            uint64_t adrp_base = DecodeADRP(adrp_addr, adrp_insn);
            for (int i = 1; i <= 4; ++i)
            {
                uint32_t insn = vm_rpm_ptr<uint32_t>((void *)(adrp_addr + (i * 4)));
                if (!IsADD(insn)) continue;
                if ((adrp_base + DecodeADD(insn)) == target_addr)
                    return adrp_addr;
            }
        }
        return 0;
    }
}

UEVarsInitStatus IGameProfile::InitUEVars()
{
    _UEVars = UEVars{};
    _cachedELF = ElfScanner{};
    _cachedNames = _cachedNamesPtr = _resolvedLegacyTable = 0;
    _cachedPID = kMgr.processID();

    bool is32Bit = KittyMemoryEx::getMapsEndWith(kMgr.processID(), "/linker64").empty();
    if (is32Bit)
    {
        if (sizeof(void *) != 4)
        {
            LOGE("当前 Dumper 为 64 位，但目标进程为 32 位，请使用对应架构版本。");
            return UEVarsInitStatus::ERROR_ARCH_MISMATCH;
        }
    }
    else
    {
        if (sizeof(void *) != 8)
        {
            LOGE("当前 Dumper 为 32 位，但目标进程为 64 位，请使用对应架构版本。");
            return UEVarsInitStatus::ERROR_ARCH_MISMATCH;
        }
    }

    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid())
    {
        LOGE("在目标进程映射中未找到有效的 UE ELF。");
        return UEVarsInitStatus::ERROR_LIB_NOT_FOUND;
    }

    if (!ArchSupprted())
    {
        if (GetUnrealELF().header().e_machine > 0 && !ue_elf.isHeaderless())
        {
            LOGE("当前游戏的架构 (0x%x) 暂不支持。", ue_elf.header().e_machine);
            return UEVarsInitStatus::ARCH_NOT_SUPPORTED;
        }
        else
        {
            LOGW("UE ELF Header might have been removed or modified!");
        }
    }

    kPtrValidator.setPID(kMgr.processID());
    kPtrValidator.setUseCache(true);
    kPtrValidator.refreshRegionCache();
    if (kPtrValidator.regions().empty())
        return UEVarsInitStatus::ERROR_INIT_PTR_VALIDATOR;

    _UEVars.BaseAddress = ue_elf.base();

    UE_Offsets *pOffsets = GetOffsets();
    if (!pOffsets)
        return UEVarsInitStatus::ERROR_INIT_OFFSETS;

    _UEVars.Offsets = pOffsets;
    _UEVars.pObjectIndex = [this](uintptr_t at) { return ReadObjectIndex(at); };
    _UEVars.pObjectClass = [this](uintptr_t at) { return ReadObjectClass(at); };
    _UEVars.pObjectOuter = [this](uintptr_t at) { return ReadObjectOuter(at); };

    _UEVars.NamesPtr = GetNamesPtr();
    if (IsUsingFNamePool())
    {
        if (!kPtrValidator.isPtrReadable(_UEVars.NamesPtr))
            return UEVarsInitStatus::ERROR_INIT_NAMEPOOL;
    }
    else
    {
        if (!kPtrValidator.isPtrReadable(_UEVars.NamesPtr))
            return UEVarsInitStatus::ERROR_INIT_GNAMES;
    }

    _UEVars.pGetNameByID = [this](int32_t id) -> std::string
    {
        return GetNameByID(id);
    };

    _UEVars.GUObjectsArrayPtr = GetGUObjectArrayPtr();
    if (!kPtrValidator.isPtrReadable(_UEVars.GUObjectsArrayPtr))
        return UEVarsInitStatus::ERROR_INIT_GUOBJECTARRAY;

    if(!BootstrapCoreObjectArrayOffsets(this,pOffsets,_UEVars.GUObjectsArrayPtr))
        return UEVarsInitStatus::ERROR_INIT_OBJOBJECTS;

    _UEVars.ObjObjectsPtr = _UEVars.GUObjectsArrayPtr + pOffsets->FUObjectArray.ObjObjects;

    if (!vm_rpm_ptr((void *)(_UEVars.ObjObjectsPtr + pOffsets->TUObjectArray.Objects),
                    &_UEVars.ObjObjects_Objects, sizeof(uintptr_t)))
        return UEVarsInitStatus::ERROR_INIT_OBJOBJECTS;
    if (!kPtrValidator.isPtrReadable(_UEVars.ObjObjects_Objects))
        return UEVarsInitStatus::ERROR_INIT_OBJOBJECTS;

    LOGI("[Bootstrap] Runtime object array: GUObject=0x%lx ObjObjects=0x%lx Objects=0x%lx",
         static_cast<unsigned long>(_UEVars.GUObjectsArrayPtr),
         static_cast<unsigned long>(_UEVars.ObjObjectsPtr),
         static_cast<unsigned long>(_UEVars.ObjObjects_Objects));

    _UEVars.Matrix = GetMatrix();
    _UEVars.Physx = GetPhysx();
    _UEVars.FrameCount = GetFrameCount();
    _UEVars.StaticFindObject = GetStaticFindObject();
    _UEVars.NativeAndroidApp = GetNativeAndroidApp();
    UEWrappers::Init(GetUEVars());
    _UEVars.ProcessEvent = GetProcessEvent();

    return UEVarsInitStatus::SUCCESS;
}

int32_t IGameProfile::ReadObjectIndex(uintptr_t address) const { return vm_rpm_ptr<int32_t>((void*)address); }
uintptr_t IGameProfile::ReadObjectClass(uintptr_t address) const { return vm_rpm_ptr<uintptr_t>((void*)address); }
uintptr_t IGameProfile::ReadObjectOuter(uintptr_t address) const { return vm_rpm_ptr<uintptr_t>((void*)address); }

uint8_t *IGameProfile::GetNameEntry(int32_t id) const
{
    if (id < 0) return nullptr;
    const auto* offsets=GetOffsets();
    const uintptr_t root=_UEVars.GetNamesPtr();
    if (!root) return nullptr;
    if (offsets->Config.IsUsingFNamePool) {
        return reinterpret_cast<uint8_t*>(UERuntime::PoolEntry(
            [](uintptr_t at,void* out,size_t size){return vm_rpm_ptr((void*)at,out,size);},root,uint32_t(id),*offsets));
    }
    const uintptr_t table=_resolvedLegacyTable ? _resolvedLegacyTable : vm_rpm_ptr<uintptr_t>((void*)root);
    if (uint32_t(id)/16384>=8192) return nullptr;
    const uintptr_t chunk=vm_rpm_ptr<uintptr_t>((void*)(table+(uint32_t(id)/16384)*sizeof(uintptr_t)));
    if(!UERuntime::Pointer(chunk)) return nullptr;
    return vm_rpm_ptr<uint8_t*>((void*)(chunk+(uint32_t(id)%16384)*sizeof(uintptr_t)));
}

std::string IGameProfile::GetNameEntryString(uint8_t* entry) const
{
    return UERuntime::Name(
        [](uintptr_t at,void* out,size_t size){return vm_rpm_ptr((void*)at,out,size);},
        reinterpret_cast<uintptr_t>(entry),*GetOffsets(),
        [this](uint32_t id){return reinterpret_cast<uintptr_t>(GetNameEntry(int32_t(id)));},
        [this](char* text,size_t n){DecryptNameAnsi(text,n);},
        [this](char16_t* text,size_t n){DecryptNameWide(text,n);});
}

std::string IGameProfile::GetNameByID(int32_t id) const
{
    return GetNameEntryString(GetNameEntry(id));
}

ElfScanner IGameProfile::GetUnrealELF() const
{
    if (_cachedPID == kMgr.processID() && _cachedELF.isValid()) return _cachedELF;
    _cachedPID = kMgr.processID();
    static const std::vector<std::string> cUELibNames = {"libUE4.so",
                                                         "libUnreal.so"};

    ElfScanner ue_elf{};
    for (const auto &lib : cUELibNames)
    {
        ue_elf = kMgr.findMemElf(lib);
        if (ue_elf.isValid())
        {
            //LOGI("[GetUnrealELF] %s found at base 0x%lx (pid=%d)",lib.c_str(), (unsigned long)ue_elf.base(), kMgr.processID());
            return _cachedELF = ue_elf;
        }
    }

    // split config
    const auto maps = KittyMemoryEx::getAllMaps(kMgr.processID());
    for (const auto &lib : cUELibNames)
    {
        for (auto &it : maps)
        {
            if (KittyUtils::String::Contains(it.pathname, kMgr.processName()) &&
                KittyUtils::String::EndsWith(it.pathname, ".apk"))
            {
                ue_elf = kMgr.findMemElfInZip(it.pathname, lib);
                if (ue_elf.isValid())
                    return _cachedELF = ue_elf;
            }
        }
    }

    // last resort, linker solist
    // some games like farlight and pubg remove ELF header from lib
    for (const auto &lib : cUELibNames)
    {
        ue_elf = kMgr.findMemElfFromLinker(lib);
        if (ue_elf.isValid())
            return _cachedELF = ue_elf;
    }

    return _cachedELF = ue_elf;
}

bool IGameProfile::isEmulator() const
{
    if (!KittyMemoryEx::getMapsContain(kMgr.processID(), "/arm/nb/").empty() ||
        !KittyMemoryEx::getMapsContain(kMgr.processID(), "/arm64/nb/").empty())
        return true;

    for (auto &it : GetUnrealELF().segments())
        if (it.executable)
            return false;

    return true;
}

uintptr_t IGameProfile::findIdaPattern(PATTERN_MAP_TYPE map_type,
                                       const std::string &pattern,
                                       const int step,
                                       uint32_t skip_result) const
{
    ElfScanner ue_elf = GetUnrealELF();
    std::vector<KittyMemoryEx::ProcMap> search_segments;
    bool hasBSS = ue_elf.bssSegments().size() > 0;

    if (map_type == PATTERN_MAP_TYPE::BSS)
    {
        if (!hasBSS)
            return 0;

        for (auto &it : ue_elf.bssSegments())
            search_segments.push_back(it);
    }
    else
    {
        for (auto &it : ue_elf.segments())
        {
            if (!it.readable || !it.is_private)
                continue;

            if (map_type == PATTERN_MAP_TYPE::ANY_X && !it.executable)
                continue;
            else if (map_type == PATTERN_MAP_TYPE::ANY_W && !it.writeable)
                continue;

            search_segments.push_back(it);
        }
    }

    LOGD("search_segments count = %p", (void *)search_segments.size());

    uintptr_t insn_address = 0;

    for (auto &it : search_segments)
    {
        if (skip_result > 0)
        {
            auto adr_list = kMgr.memScanner.findIdaPatternAll(it.startAddress,
                                                              it.endAddress, pattern);
            if (adr_list.size() > skip_result)
            {
                insn_address = adr_list[skip_result];
            }
        }
        else
        {
            insn_address = kMgr.memScanner.findIdaPatternFirst(
                it.startAddress, it.endAddress, pattern);
        }
        if (insn_address)
            break;
    }
    return (insn_address ? (insn_address + step) : 0);
}

uintptr_t IGameProfile::GetGUObjectArrayPtr() const
{
    auto* off=GetOffsets();
    const auto elf=GetUnrealELF();
    if(!off || !elf.isValid()) return 0;
    const UERuntime::Reader read=[](uintptr_t at,void* out,size_t n){return vm_rpm_ptr((void*)at,out,n);};
    const auto readable=[](uintptr_t at){return kPtrValidator.isPtrReadable(at);};
    constexpr size_t chunkSize=0x40000;
    std::vector<uint8_t> bytes(chunkSize);
    for(const auto& seg:elf.segments()) {
        if(!seg.readable || !seg.writeable || seg.executable) continue;
        for(size_t offset=0;offset<seg.length;offset+=chunkSize) {
            const auto count=std::min(chunkSize,seg.length-offset);
            if(!vm_rpm_ptr((void*)(seg.startAddress+offset),bytes.data(),count)) continue;
            for(size_t i=0;i+sizeof(uintptr_t)<=count;i+=sizeof(uintptr_t)) {
                uintptr_t value=0;std::memcpy(&value,bytes.data()+i,sizeof(value));
                if(!UERuntime::Pointer(value) || !kPtrValidator.isPtrReadable(value)) continue;
                const auto slot=seg.startAddress+offset+i;
                auto layout=UERuntime::ValidateObjectArray(read,slot,readable,
                    [this](uintptr_t at){return ReadObjectIndex(at);},uint32_t(off->FUObjectItem.Size),RelativeObjectCount(off),uint32_t(off->UObject.InternalIndex));
                if(!layout)continue;
                const auto root=slot-ObjectArrayPrefix(layout)-off->FUObjectArray.ObjObjects;
                ApplyObjectArrayLayout(off,layout,root);
                return root;
            }
        }
    }
    LOGE("[Bootstrap] No object table passed index/stride/chunk validation");
    return 0;
}

uintptr_t IGameProfile::GetNamesPtr() const
{
    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid())
    {
        LOGE("[Bootstrap] GetNamesPtr: 无效的 UE ELF (pid=%d)", kMgr.processID());
        return 0;
    }

    UE_Offsets *off = GetOffsets();
    if (!off)
        return 0;

    const bool isDeltaForce = HasAppId(this, "com.tencent.tmgp.dfm");
    if (isDeltaForce)
    {
        if (uintptr_t names = BruteForceDeltaForceNames(this, ue_elf, off))
            return names;
        LOGW("[Bootstrap] DeltaForce 专用 GNames 暴力搜索失败，回退到通用算法");
    }

    const bool preferPool=off->Config.IsUsingFNamePool;
    constexpr size_t chunkSize=0x40000;
    std::vector<uint8_t> buffer(chunkSize);
    std::unordered_set<uintptr_t> tested;
    for(int pass=0;pass<2;++pass) for(const auto& seg:ue_elf.segments()) {
        if(!seg.readable || seg.executable || (pass==0)!=seg.writeable) continue;
        for(size_t offset=0;offset<seg.length;offset+=chunkSize) {
            const auto count=std::min(chunkSize,seg.length-offset);
            if(!vm_rpm_ptr((void*)(seg.startAddress+offset),buffer.data(),count)) continue;
            for(size_t i=0;i+sizeof(uintptr_t)<=count;i+=sizeof(uintptr_t)) {
                uintptr_t value=0;std::memcpy(&value,buffer.data()+i,sizeof(value));
                if(!UERuntime::Pointer(value) || !kPtrValidator.isPtrReadable(value) || !tested.insert(value).second) continue;
                const auto slot=seg.startAddress+offset+i;
                if(preferPool && DetectNamePoolBlock(value)) {
                    if(slot<off->FNamePool.BlocksOff) continue;
                    LOGI("[Bootstrap] Validated FNamePool at 0x%lx",slot-off->FNamePool.BlocksOff);
                    return slot-off->FNamePool.BlocksOff;
                }
                if(!preferPool) if(auto table=ResolveLegacyTable(value)) {
                    _resolvedLegacyTable=table;
                    LOGI("[Bootstrap] Validated GNames slot=0x%lx table=0x%lx",slot,table);
                    return slot;
                }
            }
        }
    }

    LOGE("[Bootstrap] 自动搜索 GNames/NamePool 失败");
    return 0;
}

uintptr_t IGameProfile::GetStaticFindObject() const
{
    static constexpr char16_t kNeedle[] = u"Illegal call to StaticFindObject() while serializing object data!";

    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid()) return 0;

    uintptr_t wideStr = FindWideDataInSegments(ue_elf, kNeedle, sizeof(kNeedle) - sizeof(char16_t));
    if (!wideStr) return 0;

    std::vector<uintptr_t> adrpCandidates = FindADRPForTarget(ue_elf, wideStr);
    uintptr_t matched = FilterADRPWithADD(adrpCandidates, wideStr);
    if (!matched) return 0;

    uintptr_t fnStart = FindFunctionStart(matched, (matched > 0x1000) ? (matched - 0x1000) : 0);
    return fnStart ? fnStart : matched;
}

uintptr_t IGameProfile::GetNativeAndroidApp() const
{
    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid()) return 0;

    constexpr uintptr_t kMinAppPtr = 0x4FFFFFFFFFULL;
    constexpr uintptr_t kMaxAppPtr = 0x7FFFFFFFFFULL;
    constexpr size_t kChunk = 0x100000;
    std::vector<uint8_t> buffer(kChunk);

    for (const auto &seg : ue_elf.segments())
    {
        if (!seg.readable) continue;

        for (size_t base = 0; base < seg.length; base += kChunk)
        {
            size_t toRead = std::min<size_t>(kChunk, seg.length - base);
            if (toRead < sizeof(uintptr_t)) continue;
            if (!vm_rpm_ptr((void *)(seg.startAddress + base), buffer.data(), toRead))
                continue;

            for (size_t i = 0; i + sizeof(uintptr_t) <= toRead; i += sizeof(uintptr_t))
            {
                uintptr_t val = 0;
                memcpy(&val, buffer.data() + i, sizeof(uintptr_t));
                uintptr_t decoded = FixTaggedPtr(val);
                if (decoded < kMinAppPtr || decoded > kMaxAppPtr)
                    continue;

                uintptr_t localeHolder = FixTaggedPtr(vm_rpm_ptr<uintptr_t>((void *)(decoded + 0x20)));
                if (!kPtrValidator.isPtrReadable(localeHolder + 0x8))
                    continue;

                char tag[5] = {};
                if (!vm_rpm_ptr((void *)(localeHolder + 0x8), tag, 4))
                    continue;

                if (memcmp(tag, "zhCN", 4) == 0)
                    return seg.startAddress + base + i;
            }
        }
    }

    return 0;
}

uintptr_t IGameProfile::GetProcessEvent() const
{
    auto *objects = UEWrappers::GetObjects();
    if (!objects) return 0;

    UE_UObject firstObject = objects->GetObjectPtr(0);
    if (!firstObject) return 0;

    uintptr_t vtable = vm_rpm_ptr<uintptr_t>((void *)firstObject.GetAddress());
    if (!kPtrValidator.isPtrReadable(vtable))
        return 0;

    for (uintptr_t i = 0; i < 500; ++i)
    {
        uintptr_t function = vm_rpm_ptr<uintptr_t>((void *)(vtable + i * sizeof(uintptr_t)));
        if (!kPtrValidator.isPtrReadable(function + 0x10))
            continue;

        uint32_t insn0 = vm_rpm_ptr<uint32_t>((void *)(function));
        uint32_t insn1 = vm_rpm_ptr<uint32_t>((void *)(function + 0x4));
        uint32_t insn2 = vm_rpm_ptr<uint32_t>((void *)(function + 0x8));
        if (i > 50 && i < 100 &&
            (IsStpFpLr(insn0) || IsStrInstruction(insn0)) &&
            (IsStpFpLr(insn1) || IsStrInstruction(insn1)) &&
            (IsStpFpLr(insn2) || IsStrInstruction(insn2)))
        {
            return function;
        }
    }

    return 0;
}
