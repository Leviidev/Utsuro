// utsuro-run -- runs an Xbox One title on the NT kernel half.
//
//   utsuro-run [--trace[=substring]] <dump dir containing Mount/ and EmbeddedXvd/> [exe]
#include <map>
#include <filesystem>
#include "engine/log.h"
#include "nt/kernel.h"
#include "gpu/dxbc.h"
#include "host/input.h"
#include "host/window.h"
#include "jit/jit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void utsuro_install_crash_handler(const char* log_path);

int RunSelfTest();
namespace utsuro::nt { void InitRegistry(const std::string&); void DumpActivatableClasses(); }

using namespace utsuro;

int main(int argc, char** argv) {
    nt::KernelOptions opt;
    std::string dump;
    std::string exe;
    if (argc > 1 && std::string(argv[1]) == "--selftest") return RunSelfTest();
    if (argc > 2 && std::string(argv[1]) == "--dxbc-scan") {
        // Development aid: translate every file in a folder and count the reasons for failures.
        std::map<std::string, std::pair<int, std::string>> reasons;
        int ok = 0, failed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.find("Shader_") == std::string::npos || name.find("ResourceView") != std::string::npos) continue;
            FILE* f = std::fopen(entry.path().c_str(), "rb");
            if (!f) continue;
            std::string bytes;
            char chunk[4096];
            size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes.append(chunk, n);
            std::fclose(f);
            auto r = gpu::TranslateDxbc(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            if (r.ok) {
                ++ok;
                if (argc > 3) {   // also keep the Metal source, for compiling offline
                    if (FILE* o = std::fopen((std::string(argv[3]) + "/" + name + ".metal").c_str(), "w")) { std::fputs(r.msl.c_str(), o); std::fclose(o); }
                }
                continue;
            }
            ++failed;
            auto& slot = reasons[r.error];
            if (slot.first++ == 0) slot.second = name;
        }
        std::printf("%d translated, %d failed\n", ok, failed);
        for (const auto& [why, info] : reasons) std::printf("%5d  %s  (e.g. %s)\n", info.first, why.c_str(), info.second.c_str());
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--dxbc") {
        // Development aid: translate a DXBC file to MSL on stdout.
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) { std::perror(argv[2]); return 1; }
        std::string bytes;
        char chunk[4096];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes.append(chunk, n);
        std::fclose(f);
        auto r = gpu::TranslateDxbc(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (!r.ok) { std::fprintf(stderr, "%s: %s\n", argv[2], r.error.c_str()); return 1; }
        std::fputs(r.msl.c_str(), stdout);
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--classes") {
        nt::InitRegistry(std::string(argv[2]) + "/EmbeddedXvd/Windows/System32/config");
        nt::DumpActivatableClasses();
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--trace") opt.trace = true;
        else if (a.rfind("--trace=", 0) == 0) { opt.trace = true; opt.trace_filter = a.substr(8); }
        else if (dump.empty()) dump = a;
        else exe = a;
    }
    if (dump.empty()) {
        std::fprintf(stderr, "usage: utsuro-run [--trace[=filter]] <dump dir> [exe]\n");
        return 2;
    }
    if (const char* t = std::getenv("UTSURO_TRACE")) { opt.trace = true; opt.trace_filter = t; }
    while (dump.size() > 1 && dump.back() == '/') dump.pop_back();
    opt.system_root = dump + "/EmbeddedXvd/Windows";
    opt.title_root = dump + "/Mount";
    opt.exe_name = exe.empty() ? "Minecraft.Client.exe" : exe;

    utsuro_install_crash_handler("/tmp/utsuro-crash.log");
    if (!jit::Prewarm()) std::fprintf(stderr, "no executable memory was granted\n");
    auto run = [opt = std::move(opt)]() mutable -> int {
        nt::Kernel kernel(std::move(opt));
        std::string error;
        if (!kernel.Boot(error)) {
            std::fprintf(stderr, "boot failed: %s\n", error.c_str());
            return 1;
        }
        return kernel.Run();
    };
    input::StartControl(std::getenv("UTSURO_CTL"));
    if (std::getenv("UTSURO_HEADLESS") != nullptr) return run();
    return host::RunWindowed(run);
}
