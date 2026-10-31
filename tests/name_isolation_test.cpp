#include "UE/UEOffsets.hpp"
#include <iostream>
#include <stdexcept>
struct Names : UEVars {
    explicit Names(const std::string &name) { reset(name); }
    void reset(const std::string &name) { pGetNameByID = [name](int32_t) { return name; }; }
};
int main() {
    Names pubgm("K2_DestroyComponent"), valorant("AkCommonInitializationSettingsWithSampleRate");
    if (pubgm.GetNameByID(42) != "K2_DestroyComponent") throw std::runtime_error("first target");
    if (valorant.GetNameByID(42) != "AkCommonInitializationSettingsWithSampleRate") throw std::runtime_error("cross-target name cache contamination");
    if (pubgm.GetNameByID(42) != "K2_DestroyComponent") throw std::runtime_error("switch back");
    pubgm.reset("NewProcessName");
    if (pubgm.GetNameByID(42) != "NewProcessName") throw std::runtime_error("reinitialized process");
    std::cout << "PASS: cross-target and restarted-process name isolation (4 checks)\n";
}
