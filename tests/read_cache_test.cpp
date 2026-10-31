#include "../src/UE/ReadCache.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

static size_t checks = 0;
static void require(bool ok, const char *what)
{
    if (!ok) throw std::runtime_error(what);
    ++checks;
}

int main()
{
    constexpr uintptr_t base = 0x10000;
    std::vector<uint8_t> memory(5 * 4096);
    for (size_t i = 0; i < memory.size(); ++i) memory[i] = uint8_t(i * 17 + i / 256);
    size_t limit = memory.size();
    size_t backendCalls = 0;
    auto reader = [&](uintptr_t address, void *out, size_t size) -> size_t
    {
        ++backendCalls;
        if (address < base || address - base >= limit) return 0;
        size = std::min(size, limit - size_t(address - base));
        std::memcpy(out, memory.data() + address - base, size);
        return size;
    };
    UEMemory::PageReadCache<2> cache;
    cache.begin(true);
    std::array<uint8_t, 16> out{};
    require(cache.read(base + 20, out.data(), 8, reader), "initial read");
    require(std::memcmp(out.data(), memory.data() + 20, 8) == 0, "initial contents");
    const size_t afterFirst = backendCalls;
    require(cache.read(base + 100, out.data(), 8, reader), "same page read");
    require(backendCalls == afterFirst && cache.stats().cacheHits == 1, "same page must avoid transport");

    require(cache.read(base + 4092, out.data(), out.size(), reader), "cross-page read");
    require(std::memcmp(out.data(), memory.data() + 4092, out.size()) == 0, "cross-page bytes");
    require(cache.read(base + 2 * 4096 + 2, out.data(), 8, reader), "colliding page");
    require(cache.read(base + 20, out.data(), 8, reader), "evicted page reload");
    require(std::memcmp(out.data(), memory.data() + 20, 8) == 0, "evicted entry cannot leak old page");

    cache.begin(true);
    limit = 4096 + 6;
    require(cache.read(base + 4096, out.data(), 4, reader), "valid prefix of short page");
    std::fill(out.begin(), out.end(), 0xcc);
    require(!cache.read(base + 4098, out.data(), 8, reader), "short cached tail must fail");
    require(std::all_of(out.begin(), out.begin() + 8, [](uint8_t b) { return b == 0; }), "failed read clears output");
    require(cache.stats().shortReads > 0, "short reads counted");
    std::fill(out.begin(), out.end(), 0xcc);
    require(!cache.read(base + 4092, out.data(), out.size(), reader), "cross-page partial failure");
    require(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0; }), "partial success must not leak as object");

    cache.begin(true);
    limit = memory.size();
    auto partialPrefetch = [&](uintptr_t address, void *dest, size_t size) -> size_t
    {
        return reader(address, dest, size == 4096 ? size_t(32) : size);
    };
    require(cache.read(base + 128, out.data(), 8, partialPrefetch), "exact-read fallback after short prefetch");
    require(std::memcmp(out.data(), memory.data() + 128, 8) == 0, "exact fallback contents");

    cache.begin(true);
    require(cache.read(base + 20, out.data(), 8, reader), "session initial");
    memory[20] ^= 0xff;
    cache.end();
    cache.begin(true);
    require(cache.read(base + 20, out.data(), 8, reader) && out[0] == memory[20], "new session must observe mutations");
    cache.end();
    memory[20] ^= 0xff;
    require(cache.read(base + 20, out.data(), 8, reader) && out[0] == memory[20], "outside session must read live");

    cache.begin(false);
    require(cache.read(base + 20, out.data(), 8, reader), "disabled cache first");
    memory[20] ^= 0xff;
    require(cache.read(base + 20, out.data(), 8, reader) && out[0] == memory[20], "disabled cache stays live");
    require(cache.stats().cacheHits == 0 && cache.stats().calls == 2, "disabled stats");
    require(!cache.read(std::numeric_limits<uintptr_t>::max() - 2, out.data(), 8, reader), "address overflow rejected");
    require(!cache.read(base, nullptr, 8, reader), "null destination rejected");

    cache.begin(true);
    std::vector<uint8_t> bulk(8192);
    require(cache.read(base, bulk.data(), bulk.size(), reader), "bulk read");
    require(bulk == std::vector<uint8_t>(memory.begin(), memory.begin() + 8192), "bulk contents");
    require(cache.stats().calls == 1 && cache.stats().cacheMisses == 0, "bulk bypasses cache");
    std::cout << "read_cache_test: " << checks << " checks passed\n";
}
