#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace UEMemory
{
    struct ReadStats
    {
        uint64_t calls = 0, bytes = 0;
        uint64_t cacheHits = 0, cacheMisses = 0;
        uint64_t requests = 0, requestedBytes = 0, shortReads = 0;
    };

    // Fixed capacity, O(1) lookup, and no heap allocation per memory read.
    // The caller controls lifetime; this is a best-effort operation snapshot,
    // not a coherent snapshot of a process that keeps running.
    template <size_t SlotCount = 8192>
    class PageReadCache
    {
        static_assert(SlotCount && !(SlotCount & (SlotCount - 1)));
        static constexpr size_t PageSize = 4096;
        struct Page
        {
            uintptr_t base = 0;
            size_t valid = 0;
            std::array<uint8_t, PageSize> data;
        };
        std::unique_ptr<Page[]> pages_;
        ReadStats stats_{};

        template <typename Reader>
        size_t fetch(uintptr_t address, void *out, size_t size, Reader &reader)
        {
            ++stats_.calls;
            const size_t got = std::min(size, static_cast<size_t>(reader(address, out, size)));
            stats_.bytes += got;
            if (got != size) ++stats_.shortReads;
            return got;
        }

    public:
        void begin(bool enabled)
        {
            stats_ = {};
            pages_.reset(enabled ? new Page[SlotCount] : nullptr);
        }
        void end() { pages_.reset(); }
        ReadStats stats() const { return stats_; }

        template <typename Reader>
        bool read(uintptr_t address, void *out, size_t size, Reader reader)
        {
            ++stats_.requests;
            stats_.requestedBytes += size;
            if (!size) return true;
            if (!out || !address || size > std::numeric_limits<uintptr_t>::max() - address)
                return false;

            auto fail = [&]() { std::memset(out, 0, size); return false; };
            // Bulk operations already amortize syscalls; do not evict the hot
            // reflection pages when scanning an entire ELF or object table.
            if (!pages_ || size > PageSize)
                return fetch(address, out, size, reader) == size ? true : fail();

            auto *dest = static_cast<uint8_t *>(out);
            for (size_t done = 0; done < size;)
            {
                const uintptr_t current = address + done;
                const uintptr_t base = current & ~uintptr_t(PageSize - 1);
                const size_t offset = current - base;
                const size_t count = std::min(size - done, PageSize - offset);
                const uintptr_t pageNumber = base / PageSize;
                Page &page = pages_[(pageNumber ^ (pageNumber >> 13)) & (SlotCount - 1)];
                if (page.base == base && page.valid >= offset + count)
                {
                    ++stats_.cacheHits;
                }
                else
                {
                    ++stats_.cacheMisses;
                    page.base = base;
                    page.valid = fetch(base, page.data.data(), PageSize, reader);
                    if (page.valid < offset + count)
                    {
                        // A large prefetch can fail at a boundary even though
                        // the requested subrange is readable. Never serve an
                        // unread tail or turn a short read into success.
                        if (fetch(current, dest + done, count, reader) != count)
                            return fail();
                        done += count;
                        continue;
                    }
                }
                std::memcpy(dest + done, page.data.data() + offset, count);
                done += count;
            }
            return true;
        }
    };
}
