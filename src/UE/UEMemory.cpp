#include "UEMemory.hpp"

#include <fcntl.h>
#include <mutex>
#include <atomic>
#include <array>
#include <limits>

namespace UEMemory
{
    KittyMemoryMgr kMgr{};
    KittyPtrValidator kPtrValidator;

    namespace
    {
        std::atomic<bool> cacheEnabled{true};
        thread_local PageReadCache<> readCache;
        thread_local unsigned sessionDepth = 0;
        thread_local uint64_t sessionGeneration = 0;
        thread_local bool sessionCached = false;
    }

    void SetCacheEnabled(bool enabled) { cacheEnabled.store(enabled, std::memory_order_relaxed); }
    bool IsReadSessionActive() { return sessionDepth && sessionCached; }
    uint64_t GetReadSessionGeneration() { return sessionGeneration; }
    ReadStats GetReadStats() { return readCache.stats(); }
    void BeginReadSession(bool enabled)
    {
        if (sessionDepth++ != 0) return;
        ++sessionGeneration;
        sessionCached = enabled && cacheEnabled.load(std::memory_order_relaxed);
        readCache.begin(sessionCached);
    }
    void EndReadSession()
    {
        if (sessionDepth && --sessionDepth == 0)
        {
            readCache.end();
            sessionCached = false;
        }
    }

    bool vm_rpm_ptr(const void *address, void *result, size_t len)
    {
        return readCache.read(uintptr_t(address), result, len,
            [](uintptr_t source, void *dest, size_t size) -> size_t
            {
                if (!kPtrValidator.isPtrReadable(source, 1)) return 0;
                return kMgr.readMem(source, dest, size);
            });
    }

    std::string vm_rpm_str(const void *address, size_t max_len)
    {
        std::string str;
        std::array<char, 256> chunk;
        for (size_t done = 0; done < max_len;)
        {
            const uintptr_t at = uintptr_t(address) + done;
            const size_t count = std::min({chunk.size(), max_len - done, size_t(4096 - (at & 4095))});
            if (!vm_rpm_ptr(reinterpret_cast<void *>(at), chunk.data(), count)) return "";
            const auto *nul = static_cast<const char *>(std::memchr(chunk.data(), 0, count));
            str.append(chunk.data(), nul ? size_t(nul - chunk.data()) : count);
            if (nul) break;
            done += count;
        }
        return str;
    }

    std::wstring vm_rpm_strw(const void *address, size_t max_len)
    {
        // Unreal stores UTF-16, whereas Android wchar_t is 32 bits.
        std::wstring str;
        std::array<uint16_t, 128> chunk;
        for (size_t done = 0; done < max_len;)
        {
            const uintptr_t at = uintptr_t(address) + done * sizeof(uint16_t);
            const size_t count = std::min({chunk.size(), max_len - done, std::max(size_t(1), size_t(4096 - (at & 4095)) / 2)});
            if (!vm_rpm_ptr(reinterpret_cast<void *>(at), chunk.data(), count * 2)) return L"";
            for (size_t i = 0; i < count; ++i)
            {
                if (!chunk[i]) return str;
                str.push_back(static_cast<wchar_t>(chunk[i]));
            }
            done += count;
        }
        return str;
    }

    uintptr_t FindAlignedPointerRefrence(uintptr_t start, size_t range, uintptr_t ptr)
    {
        if (start == 0 || start != GetPtrAlignedOf(start))
            return 0;

        if (range < sizeof(void *) || range != GetPtrAlignedOf(range))
            return 0;

        if (range > std::numeric_limits<uintptr_t>::max() - start) return 0;
        std::array<uintptr_t, (64 * 1024) / sizeof(uintptr_t)> values;
        for (size_t offset = 0; offset < range;)
        {
            const size_t count = std::min(sizeof(values), range - offset);
            if (vm_rpm_ptr(reinterpret_cast<void *>(start + offset), values.data(), count))
            {
                for (size_t i = 0; i < count / sizeof(uintptr_t); ++i)
                    if (values[i] == ptr) return start + offset + i * sizeof(uintptr_t);
            }
            else
            {
                // A mapping may disappear during a scan. Keep readable pages.
                for (size_t sub = 0; sub < count;)
                {
                    const uintptr_t at = start + offset + sub;
                    const size_t bytes = std::min(count - sub, size_t(4096 - (at & 4095)));
                    if (vm_rpm_ptr(reinterpret_cast<void *>(at), values.data(), bytes))
                        for (size_t i = 0; i < bytes / sizeof(uintptr_t); ++i)
                            if (values[i] == ptr) return at + i * sizeof(uintptr_t);
                    sub += bytes;
                }
            }
            offset += count;
        }
        return 0;
    }

    namespace Arm64
    {
        namespace
        {
            bool decode_adrl_target(uint32_t insn, uintptr_t insn_address, uint32_t &rd, uintptr_t &target)
            {
                int64_t pc_rel = 0;
                if (!KittyArm64::decode_adr_imm(insn, &pc_rel))
                    return false;

                if (!KittyArm64::is_insn_adr(insn) && !KittyArm64::is_insn_adrp(insn))
                    return false;

                rd = KittyArm64::bits_from(insn, 0, 5);
                if (KittyArm64::is_insn_adrp(insn))
                    target = kINSN_PAGE_OFFSET(insn_address) + pc_rel;
                else
                    target = insn_address + pc_rel;

                return true;
            }

            bool decode_adrl_imm(uint32_t insn, uint32_t expected_rn, intptr_t &imm)
            {
                const uint32_t rn = KittyArm64::bits_from(insn, 5, 5);
                if (rn != expected_rn)
                    return false;

                // add/sub (immediate)
                if ((insn & 0x1F800000) == 0x11000000)
                {
                    imm = KittyArm64::decode_addsub_imm(insn);
                    if (KittyArm64::bit_from(insn, 30))
                        imm = -imm;

                    return imm != 0;
                }

                int32_t uimm = 0;
                if (KittyArm64::decode_ldrstr_uimm(insn, &uimm))
                {
                    imm = uimm;
                    return imm != 0;
                }

                return false;
            }
        }  // namespace

        uintptr_t DecodeADRL(uintptr_t adrp_address, uint32_t imm_insn_offset)
        {
            if (adrp_address == 0) return 0;

            uint32_t adrp_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address));
            if (adrp_insn == 0)
                return 0;

            uint32_t adrp_rd = 0;
            uintptr_t adrp_target = 0;
            if (!decode_adrl_target(adrp_insn, adrp_address, adrp_rd, adrp_target))
                return 0;

            if (imm_insn_offset == 0)
            {
                // scan the next few instructions for a base-register immediate use
                for (int i = 1; i < 8; i++)
                {
                    uint32_t imm_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address + (i * 4)));
                    intptr_t imm = 0;
                    if (imm_insn != 0 && decode_adrl_imm(imm_insn, adrp_rd, imm))
                        return adrp_target + imm;
                }
            }
            else
            {
                uint32_t imm_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address + imm_insn_offset));
                if (imm_insn == 0)
                    return 0;

                intptr_t imm = 0;
                if (decode_adrl_imm(imm_insn, adrp_rd, imm))
                    return adrp_target + imm;
            }

            return 0;
        }

        uintptr_t Decode_ADRP_ADD(uintptr_t adrp_address, uint32_t add_offset)
        {
            if (adrp_address == 0) return 0;

            const uintptr_t page_off = kINSN_PAGE_OFFSET(adrp_address);

            int64_t adrp_pc_rel = 0;
            int32_t add_imm12 = 0;

            uint32_t adrp_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address));
            uint32_t add_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address + add_offset));
            if (adrp_insn == 0 || add_insn == 0)
                return 0;

            if (!KittyArm64::decode_adr_imm(adrp_insn, &adrp_pc_rel) || adrp_pc_rel == 0)
                return 0;

            add_imm12 = KittyArm64::decode_addsub_imm(add_insn);

            return (page_off + adrp_pc_rel + add_imm12);
        }

        uintptr_t Decode_ADRP_LDR(uintptr_t adrp_address, uint32_t ldr_offset)
        {
            if (adrp_address == 0) return 0;

            const uintptr_t page_off = kINSN_PAGE_OFFSET(adrp_address);

            int64_t adrp_pc_rel = 0;
            int32_t ldr_imm12 = 0;

            uint32_t adrp_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address));
            uint32_t ldr_insn = vm_rpm_ptr<uint32_t>((void *)(adrp_address + ldr_offset));
            if (adrp_insn == 0 || ldr_insn == 0)
                return 0;

            if (!KittyArm64::decode_adr_imm(adrp_insn, &adrp_pc_rel) || adrp_pc_rel == 0)
                return 0;

            if (!KittyArm64::decode_ldrstr_uimm(ldr_insn, &ldr_imm12))
                return 0;

            return (page_off + adrp_pc_rel + ldr_imm12);
        }


    }  // namespace Arm64

}  // namespace UEMemory

namespace IOUtils
{
    std::string get_filename(const std::string &filePath)
    {
        std::string filename;
        const size_t last_slash_idx = filePath.find_last_of("/\\");
        if (std::string::npos != last_slash_idx)
        {
            filename = filePath.substr(last_slash_idx + 1);
        }
        return filename;
    }

    std::string get_file_directory(const std::string &filePath)
    {
        std::string directory;
        const size_t last_slash_idx = filePath.find_last_of("/\\");
        if (std::string::npos != last_slash_idx)
        {
            directory = filePath.substr(0, last_slash_idx);
        }
        return directory;
    }

    std::string get_file_extension(const std::string &filePath)
    {
        std::string ext;
        const size_t last_slash_idx = filePath.find_last_of(".");
        if (std::string::npos != last_slash_idx)
        {
            ext = filePath.substr(last_slash_idx + 1);
        }
        return ext;
    }

    bool file_path_contains(const std::string &filePath, const std::string &subPath)
    {
        return (!filePath.empty() && (filePath.find(subPath) != std::string::npos));
    }

    std::string remove_specials(std::string s)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            if (!((s[i] < 'A' || s[i] > 'Z') && (s[i] < 'a' || s[i] > 'z')))
                continue;

            if (!(s[i] < '0' || s[i] > '9'))
                continue;

            if (s[i] == '_')
                continue;

            s.erase(s.begin() + i);
            --i;
        }
        return s;
    }

    std::string replace_specials(std::string s, char c)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            if (!((s[i] < 'A' || s[i] > 'Z') && (s[i] < 'a' || s[i] > 'z')))
                continue;

            if (!(s[i] < '0' || s[i] > '9'))
                continue;

            if (s[i] == '_')
                continue;

            s[i] = c;
        }
        return s;
    }

    void delete_directory(const std::string &directory)
    {
        DIR *dp;
        struct dirent *rd;

        dp = opendir(directory.c_str());
        if (!dp)
            return;

        while ((rd = readdir(dp)) != NULL)
        {
            if (!strcmp(rd->d_name, ".") || !strcmp(rd->d_name, ".."))
                continue;

            std::string path = directory + '/' + rd->d_name;
            if (path_is_directory(path.c_str()))
                delete_directory(path.c_str());
            else
                unlink(path.c_str());
        }

        closedir(dp);
        rmdir(directory.c_str());
    }

    int path_is_directory(const std::string &path)
    {
        struct stat st;

        if (stat(path.c_str(), &st))
            return 0;

        return S_ISDIR(st.st_mode);
    }

    int mkdir_recursive(const std::string &dirPath, mode_t mode)
    {
        std::string tmp = dirPath;
        for (char *p = strchr(tmp.data() + 1, '/'); p; p = strchr(p + 1, '/'))
        {
            *p = '\0';
            errno = 0;
            if (mkdir(tmp.c_str(), mode) == -1)
            {
                if (errno != EEXIST)
                {
                    *p = '/';
                    return -1;
                }
            }
            *p = '/';
        }
        errno = 0;
        return mkdir(tmp.c_str(), mode);
    }
}  // namespace IOUtils
