#pragma once
#include <atomic>
#include <string>
int RunHttpServer(unsigned short port, const std::string& webRoot, std::atomic_bool& stop);
