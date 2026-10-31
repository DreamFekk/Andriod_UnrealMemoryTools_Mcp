#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>

namespace FastDump {
struct Result {
    bool ok = false;
    uint64_t bytes = 0, readCalls = 0;
    double milliseconds = 0;
    std::string error;
};
// The output uses the original memory-image layout: byte zero is `start`.
// Incomplete reads are errors, never silently substituted with zeroes.
Result Range(pid_t pid, uintptr_t start, uintptr_t end, const std::string& path,
             const std::function<void(uint64_t, uint64_t)>& progress = {}, unsigned workers = 4);
}
