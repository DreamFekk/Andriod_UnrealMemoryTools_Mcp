#pragma once
#include "AutoFix.hpp"
#include "../UERuntimeSupport.hpp"

// Algorithm provenance: MobileDumper-7 Shared/NewState.h, MJx0 / droidxrx.
// Keys match that reference revision; validation against the target is required.
class NewStateProfile final : public AutoFixProfile {
public:
    NewStateProfile() : AutoFixProfile("com.pubg.newstate") {}
    std::string GetAppName() const override {return "NEW STATE Mobile";}
    std::vector<std::string> GetAppIDs() const override {return {"com.pubg.newstate"};}
    bool RequireValidatedLayout() const override {return true;}
    int32_t ReadObjectIndex(uintptr_t at) const override {
        std::array<uint32_t,3> data{};
        if(!UEMemory::vm_rpm_ptr((void*)at,data.data(),sizeof(data))) return -1;
        return int32_t(UERuntime::NewStateIndex(data));
    }
    uintptr_t ReadObjectClass(uintptr_t at) const override {
        std::array<uint64_t,3> data{};
        if(!UEMemory::vm_rpm_ptr((void*)at,data.data(),sizeof(data))) return 0;
        return UERuntime::NewStatePointer(data);
    }
    uintptr_t ReadObjectOuter(uintptr_t at) const override {return ReadObjectClass(at);}
protected:
    void DecryptNameAnsi(char* text,size_t n) const override {UERuntime::NewStateNames(text,n);}
    void DecryptNameWide(char16_t* text,size_t n) const override {UERuntime::NewStateNames(text,n);}
    // These optional game-specific extras are unrelated to reflection discovery.
    uintptr_t GetMatrix() const override {return 0;}
    uintptr_t GetPhysx() const override {return 0;}
    uintptr_t GetFrameCount() const override {return 0;}
};
