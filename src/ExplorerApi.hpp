#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
namespace ExplorerApi {
// Caller holds the UE operation mutex and owns a scoped read session.
nlohmann::json Read(const nlohmann::json& request);
std::string PropertyValue(uintptr_t instance,uintptr_t descriptor,bool modern);
}
