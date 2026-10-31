#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../UEGameProfile.hpp"
using namespace UEMemory;
class WutheringWavesProfile : public IGameProfile
{
    struct KuroRec
    {
        uintptr_t addr;
        uint16_t hdr;
    };

    // App::Probe creates a fresh profile for each probe, including the same PID.
    mutable pid_t sPid = 0;
    mutable std::unordered_map<uint32_t, KuroRec> sIdx;

public:
    WutheringWavesProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64 || e_machine == EM_ARM;
    }

    std::string GetAppName() const override
    {
        return "Wuthering Waves";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.kurogame.mingchao", "com.kurogame.wutheringwaves.global"};
    }

    bool isUsingCasePreservingName() const override
    {
        return true;
    }

    bool IsUsingFNamePool() const override
    {
        return true;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

    uintptr_t GetMatrix() const override { return 0; }
    uintptr_t GetPhysx() const override { return 0; }
    uintptr_t GetFrameCount() const override { return 0; }

    uintptr_t GetGUObjectArrayPtr() const override
    {
        uintptr_t guobjectarray = GetUnrealELF().findSymbol("GUObjectArray");
        if (guobjectarray != 0)
            return guobjectarray;

        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"91 E1 03 ? AA E0 03 08 AA E2 03 1F 2A", -7},
            {"B4 21 0C 40 B9 ? ? ? ? ? ? ? 91", 5},
            {"9F E5 00 ? 00 E3 FF ? 40 E3 ? ? A0 E1", -2},
            {"96 df 02 17 ? ? ? ? 54 ? ? ? ? ? ? ? 91 e1 03 13 aa", 9},
            {"f4 03 01 2a ? 00 00 34 ? ? ? ? ? ? ? ? ? ? 00 54 ? 00 00 14 ? ? ? ? ? ? ? 91", 0x18},
            {"69 3e 40 b9 1f 01 09 6b ? ? ? 54 e1 03 13 aa ? ? ? ? f4 4f ? a9 ? ? ? ? ? ? ? 91", 0x18},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }

        return 0;
    }

    uintptr_t GetNamesPtr() const override
    {
        UE_Offsets *offs = GetOffsets();
        uintptr_t namesPtr = 0;
        uintptr_t blockBase = 0;
        uintptr_t blocks_p = GetUnrealELF().findSymbol("GNameBlocksDebug");
        if (blocks_p != 0)
        {
            blocks_p = vm_rpm_ptr<uintptr_t>((void *)blocks_p);
            if (blocks_p != 0)
            {
                blockBase = blocks_p;
                namesPtr = blocks_p - offs->FNamePool.BlocksOff;
            }
        }
        if (namesPtr == 0)
        {
            std::vector<std::pair<std::string, int>> idaPatterns = {
                {"F4 4F 01 A9 FD 7B 02 A9 FD 83 00 91 ? ? ? ? ? ? ? ? A8 02 ? 39", 0x18},
                {"F4 4F 01 A9 FD 7B 02 A9 FD 83 00 91 ? ? ? ? A8 02 ? 39", 0x24},
                {"fd 7b 01 a9 fd 43 00 91 ? ? ? ? 89 ? ? 39 f3 03 08 aa c9 00 00 37 ? ? ? ? ? ? ? 91", 0x18},
                {"02 ? 91 C8 00 00 37 ? ? ? ? ? ? ? 91", 7},
                {"39 C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97 ? 00 80 52 ? ? ? 39", 5},
                {"C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97 ? 00 80 52", 4},
                {"C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97", 4},
            };

            PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

            for (const auto &it : idaPatterns)
            {
                uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, it.first, it.second));
                if (adrl != 0)
                {
                    namesPtr = adrl;
                    blockBase = namesPtr + offs->FNamePool.BlocksOff;
                    break;
                }
            }
        }
        if (namesPtr != 0)
        {
            static bool sKuroDiagDone = true;
            if (!sKuroDiagDone)
            {
                sKuroDiagDone = true;

                LOGI("[KuroDiag] NamesPtr=0x%lX blockBase=0x%lX BlocksBit=%lu BlocksOff=0x%lX Stride=%lu",
                     (unsigned long)namesPtr, (unsigned long)blockBase,
                     (unsigned long)offs->FNamePool.BlocksBit,
                     (unsigned long)offs->FNamePool.BlocksOff,
                     (unsigned long)offs->FNamePool.Stride);

                uintptr_t chunk0 = blockBase ? vm_rpm_ptr<uintptr_t>((void *)blockBase) : 0;

                const int32_t probeIds[] = {0, 1, 2, 3, 4, 5, 1002, 1003};
                const uint32_t blockBit = offs->FNamePool.BlocksBit;
                const uintptr_t stride = offs->FNamePool.Stride;
                const uintptr_t mask = blockBit ? ((uintptr_t(1) << blockBit) - 1) : 0;

                for (int32_t id : probeIds)
                {
                    const uintptr_t blockOff = (uint64_t(id) >> blockBit) * sizeof(void *);
                    uintptr_t chunk = blockBase ? vm_rpm_ptr<uintptr_t>((void *)(blockBase + blockOff)) : 0;
                    uintptr_t entry = 0;
                    if (chunk)
                        entry = chunk + (uintptr_t(id) & mask) * stride;

                    uint8_t raw[64] = {};
                    if (!entry || !vm_rpm_ptr((void *)entry, raw, sizeof(raw)))
                    {
                        LOGI("[KuroDiag] id=%d entry 不可读", id);
                        continue;
                    }

                    char hex[16 * 3 + 1] = {0};
                    char asc[16 + 1] = {0};
                    for (int i = 0; i < 16; i++)
                    {
                        snprintf(hex + i * 3, 4, "%02X ", raw[i]);
                        asc[i] = (raw[i] >= 0x20 && raw[i] < 0x7f) ? (char)raw[i] : '.';
                    }
                    asc[16] = 0;

                    std::string run;
                    for (int i = 0; i < 48 && (int)run.size() < 40; i++)
                    {
                        const char c = (char)raw[i];
                        if (c >= 0x20 && c < 0x7f)
                            run.push_back(c);
                        else if (!run.empty())
                            run.push_back('|');
                    }

                    LOGI("[KuroDiag] id=%d entry=0x%lX | %s | %s", id,
                         (unsigned long)entry, hex, asc);
                    LOGI("[KuroDiag] id=%d 前48字节可读: '%s'", id, run.c_str());
                }
                if (chunk0)
                {
                    constexpr size_t kScan = 0x40000;  // 256KB
                    std::vector<uint8_t> buf(kScan, 0);
                    size_t readOk = 0;
                    for (size_t off = 0; off < kScan; off += 0x10000)
                    {
                        const size_t want = std::min<size_t>(0x10000, kScan - off);
                        if (!vm_rpm_ptr((void *)(chunk0 + off), buf.data() + off, want))
                            break;
                        readOk += want;
                    }

                    if (readOk < 0x10000)
                    {
                        LOGI("[KuroDiag] 块扫描读不到 (readOk=%zu)", readOk);
                    }
                    else
                    {
                        const uint32_t targets[] = {0, 3, 1002, 1003, 1004};
                        for (uint32_t tg : targets)
                        {
                            int hits = 0;
                            for (size_t i = 0; i + 8 < readOk && hits < 2; i += 4)
                            {
                                uint32_t v = 0;
                                memcpy(&v, buf.data() + i, sizeof(v));
                                if (v != tg)
                                    continue;
                                hits++;

                                const uint16_t len2 = static_cast<uint16_t>(buf[i + 4] | (buf[i + 5] << 8));
                                const int wantChars = len2 >> 1;
                                std::string nm;
                                for (int k = 0; k < wantChars && (i + 6 + (size_t)k) < readOk; k++)
                                {
                                    const unsigned char c = buf[i + 6 + k];
                                    if (c < 0x20 || c >= 0x7f)
                                        break;
                                    nm.push_back((char)c);
                                }
                                LOGI("[KuroDiag] ★ dword==%u @ 块内+0x%lX len2=%u -> '%s'",
                                     tg, (unsigned long)i, len2, nm.c_str());
                            }
                        }
                    }
                }
            }
        }

        return namesPtr;
    }

    std::string GetNameByID(int32_t id) const override
    {
        if (id < 0)
            return "";

        const pid_t curPid = kMgr.processID();
        if (curPid != sPid)
        {
            sPid = curPid;
            sIdx.clear();

            uintptr_t blockTable = 0;
            const uintptr_t blocksSym = GetUnrealELF().findSymbol("GNameBlocksDebug");
            if (blocksSym != 0)
                blockTable = vm_rpm_ptr<uintptr_t>((void *)blocksSym);
            // GetNamesPtr also supports stripped builds via instruction patterns.
            if (!blockTable && _UEVars.GetNamesPtr())
                blockTable = _UEVars.GetNamesPtr() + GetOffsets()->FNamePool.BlocksOff;

            constexpr size_t kMaxBytesPerBlock = 0x800000;
            constexpr size_t kStep = 0x10000;

            for (uintptr_t bi = 0; bi < 1024 && blockTable; ++bi)
            {
                const uintptr_t chunkAddr =
                    vm_rpm_ptr<uintptr_t>((void *)(blockTable + bi * sizeof(void *)));
                if (!chunkAddr)
                    break;
                if (sIdx.size() > 600000)
                    break;

                std::vector<uint8_t> mem;
                mem.reserve(kMaxBytesPerBlock);
                size_t got = 0;
                bool readOkAll = true;
                for (size_t off = 0; off < kMaxBytesPerBlock && readOkAll; off += kStep)
                {
                    std::vector<uint8_t> piece(kStep);
                    if (!vm_rpm_ptr((void *)(chunkAddr + off), piece.data(), piece.size()))
                    {
                        if (off == 0)
                            break;
                        readOkAll = false;
                        break;
                    }
                    mem.insert(mem.end(), piece.begin(), piece.end());
                    got += kStep;
                }

                if (got < 6)
                    continue;

                size_t p = 0;
                size_t badRun = 0;
                while (p + 6 <= got && badRun < 8)
                {
                    uint32_t rid = 0;
                    memcpy(&rid, mem.data() + p, sizeof(rid));
                    const uint16_t hdr = static_cast<uint16_t>(mem[p + 4] | (mem[p + 5] << 8));
                    const bool wide = (hdr & 1) != 0;
                    const uint32_t chars = hdr >> 1;

                    if ((rid == 0 && chars == 0) || chars == 0 || chars > 4096)
                    {
                        ++badRun;
                        p += 4;
                        continue;
                    }
                    badRun = 0;

                    const size_t nameBytes = wide ? size_t(chars) * 2 : size_t(chars);
                    const size_t total = 6 + nameBytes;
                    if (p + total > got)
                        break;

                    if (sIdx.find(rid) == sIdx.end())
                    {
                        KuroRec rc;
                        rc.addr = chunkAddr + p;
                        rc.hdr = hdr;
                        sIdx[rid] = rc;
                    }

                    p += (total + 3) & ~size_t(3);  // 4 字节对齐
                }
            }

            LOGI("[Kuro] FName 索引建立完成: %zu 条", sIdx.size());
        }

        const auto it = sIdx.find(static_cast<uint32_t>(id));
        if (it == sIdx.end())
            return "";

        const KuroRec &rc = it->second;
        const bool wide = (rc.hdr & 1) != 0;
        const uint32_t chars = rc.hdr >> 1;

        if (wide)
        {
            std::vector<uint16_t> w(chars);
            if (!vm_rpm_ptr((void *)(rc.addr + 6), w.data(), chars * sizeof(uint16_t)))
                return "";
            std::string out;
            out.reserve(chars);
            for (uint32_t i = 0; i < chars; ++i)
            {
                const uint16_t c = w[i];
                out.push_back(c < 0x80 ? (char)c : '?');
            }
            return out;
        }
        else
        {
            std::vector<char> a(chars);
            if (!vm_rpm_ptr((void *)(rc.addr + 6), a.data(), chars))
                return "";
            return std::string(a.data(), chars);
        }
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        return &offsets;
    }
};
