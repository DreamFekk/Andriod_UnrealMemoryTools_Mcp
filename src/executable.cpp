#include "App.hpp"
#include "HttpServer.hpp"
#include "UE/UEMemory.hpp"
#ifdef UMT_NATIVE_UI
#include "NativeUI.hpp"
#endif
#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
std::atomic_bool stop{false};
void Interrupt(int) { stop.store(true); }
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    signal(SIGPIPE,SIG_IGN);
    signal(SIGINT,Interrupt);
    signal(SIGTERM,Interrupt);
    std::string output="/sdcard/UnrealMemoryTools_Refactored", web="/data/local/tmp/umt-refactored/web", action;
    unsigned port=18765;
    int pid=0;
    bool serve=false, cache=true, explorer=false,worlds=false;
    try {
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            auto value=[&]() -> std::string { if (++i>=argc) throw std::runtime_error("Missing value for "+key); return argv[i]; };
            if (key=="--serve") serve=true;
            else if (key=="--explorer") explorer=true;
            else if (key=="--worlds") worlds=true;
            else if (key=="--port") { port=std::stoul(value()); if(!port || port>65535) throw std::runtime_error("Invalid port"); }
            else if (key=="--web") web=value();
            else if (key=="--output") output=value();
            else if (key=="--pid") { pid=std::stoi(value()); if(pid<=0) throw std::runtime_error("Invalid PID"); }
            else if (key=="--action") action=value();
            else if (key=="--no-cache") cache=false;
            else if (key=="--help") {
                std::cout << "UnrealMemoryTools 1.0.1\n"
                    "  (no args)           Native phone overlay\n"
                    "  --serve             Phone/desktop web interface via ADB\n"
                    "  --pid N --action probe|dump|library|all  Headless operation\n"
                    "  --action list       List Unreal processes\n"
                    "  --explorer --pid N  Probe then open native SDK explorer\n"
                    "  --worlds --pid N    Probe then open native world Actors\n"
                    "  --output PATH --web PATH --port 18765 --no-cache\n";
                return 0;
            } else throw std::runtime_error("Unknown argument: "+key);
        }
        UEMemory::SetCacheEnabled(cache);
        App::Init(output);
        int result=0;
        if (serve) result=RunHttpServer(static_cast<unsigned short>(port),web,stop);
        else if (action=="list") std::cout << App::Processes().dump(2) << '\n';
        else if (!action.empty()) {
            if (pid<=0) throw std::runtime_error("--pid is required");
            if (action!="probe" && action!="dump" && action!="library" && action!="all") throw std::runtime_error("Unknown action");
            auto run=[&](const char* name) {
                const auto accepted=App::Action({{"action",name},{"pid",pid}});
                if (!accepted.value("accepted",false)) throw std::runtime_error(accepted.dump());
                App::Wait();
                const auto state=App::State();
                std::cout << "UMT_RESULT=" << state.dump(-1,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
                if (!state.value("error",std::string()).empty()) throw std::runtime_error(state.at("error").get<std::string>());
            };
            run("probe");
            if (action=="dump" || action=="all") run("dump");
            if (action=="library" || action=="all") run("library");
        } else {
#ifdef UMT_NATIVE_UI
            if(explorer || worlds) {
                if(pid<=0) throw std::runtime_error("--explorer / --worlds requires --pid");
                const auto accepted=App::Action({{"action","probe"},{"pid",pid}});
                if(!accepted.value("accepted",false)) throw std::runtime_error(accepted.dump());
                App::Wait();
                const auto state=App::State();
                if(!state.value("probed",false)) throw std::runtime_error(state.value("error",std::string("Probe failed")));
            }
            result=RunNativeUI(explorer,worlds);
#else
            throw std::runtime_error("This build is headless: use --serve or --action");
#endif
        }
        App::Shutdown();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        App::Shutdown();
        return 1;
    }
}
