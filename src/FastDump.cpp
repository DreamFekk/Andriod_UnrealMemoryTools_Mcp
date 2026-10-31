#include "FastDump.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace FastDump {
Result Range(pid_t pid, uintptr_t start, uintptr_t end, const std::string& path,
             const std::function<void(uint64_t, uint64_t)>& progress, unsigned workers) {
    Result result;
    const auto begin = std::chrono::steady_clock::now();
    if (pid <= 0 || end <= start || end - start > (uint64_t{1} << 36)) {
        result.error = "Invalid memory range"; return result;
    }
    const int mem = open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
    if (mem < 0) { result.error = std::string("open mem: ") + strerror(errno); return result; }
    const auto temporary = path + ".partial";
    const int out = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { result.error = std::string("open output: ") + strerror(errno); close(mem); return result; }
    const uint64_t total = end - start;
    constexpr size_t block = 2 * 1024 * 1024;
    std::atomic<uint64_t> next{0}, written{0}, calls{0};
    std::atomic_bool failed{false};
    std::mutex errorLock;
    auto fail = [&](const char* operation, uint64_t offset) {
        const int code = errno;
        std::lock_guard lock(errorLock);
        if (!failed.exchange(true)) result.error = std::string(operation) + " at +" + std::to_string(offset) + ": " + strerror(code);
    };
    auto copy = [&] {
        std::vector<char> buffer(block);
        while (!failed) {
            const auto offset = next.fetch_add(block);
            if (offset >= total) break;
            const size_t length = std::min<uint64_t>(block, total - offset);
            size_t done = 0;
            while (done < length && !failed) {
                ++calls;
                const auto n = pread64(mem, buffer.data() + done, length - done, start + offset + done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { if (!n) errno = EIO; fail("read", offset + done); break; }
                done += static_cast<size_t>(n);
            }
            if (failed) break;
            done = 0;
            while (done < length && !failed) {
                const auto n = pwrite64(out, buffer.data() + done, length - done, offset + done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { if (!n) errno = EIO; fail("write", offset + done); break; }
                done += static_cast<size_t>(n);
            }
            if (!failed) written.fetch_add(length);
        }
    };
    std::vector<std::thread> pool;
    const unsigned count = std::clamp(workers, 1u, 4u);
    std::atomic<unsigned> active{count};
    std::mutex completionMutex;
    std::condition_variable completion;
    for (unsigned i = 0; i < count; ++i) pool.emplace_back([&] {
        try { copy(); } catch (const std::exception&) { errno = ENOMEM; fail("copy", 0); }
        --active;
        completion.notify_one();
    });
    while (active) {
        if (progress) progress(written.load(), total);
        std::unique_lock lock(completionMutex);
        completion.wait_for(lock,std::chrono::milliseconds(40),[&]{return active.load()==0;});
    }
    for (auto& thread : pool) thread.join();
    if (!failed && fsync(out) != 0) fail("fsync", written);
    if (close(out) != 0 && !failed) fail("close", written);
    close(mem);
    if (!failed && rename(temporary.c_str(), path.c_str()) != 0) fail("rename", written);
    result.bytes = written;
    result.readCalls = calls;
    result.ok = !failed && written == total;
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    if (progress) progress(written, total);
    return result;
}
}
