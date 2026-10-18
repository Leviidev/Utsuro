// utsuro-run -- runs an Xbox One title on the NT kernel half.
//
//   utsuro-run [--trace[=substring]] <dump dir containing Mount/ and EmbeddedXvd/> [exe]
#include "engine/log.h"
#include "nt/kernel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void utsuro_install_crash_handler(const char* log_path);

int RunSelfTest();

using namespace utsuro;

int main(int argc, char** argv) {
    nt::KernelOptions opt;
    std::string dump;
    std::string exe;
    if (argc > 1 && std::string(argv[1]) == "--selftest") return RunSelfTest();
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
    nt::Kernel kernel(std::move(opt));
    std::string error;
    if (!kernel.Boot(error)) {
        std::fprintf(stderr, "boot failed: %s\n", error.c_str());
        return 1;
    }
    return kernel.Run();
}
