#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace App {
using json = nlohmann::json;
void Init(const std::string& outputRoot);
json Processes();
json State();
json Action(const json& request);
json Explore(const json& request);
json Files();
std::string ResolveFile(const std::string& name);
void Wait();
void Shutdown();
void RenderExplorer(bool world = false);
}
