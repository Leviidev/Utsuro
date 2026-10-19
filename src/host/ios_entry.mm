// ios_entry.mm -- what the iOS app calls where the Mac has main() and a window: the app owns the view, this runs the title.
#include "engine/log.h"
#include "gpu/renderer.h"
#include "host/input.h"
#include "jit/jit.h"
#include "nt/kernel.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

extern "C" void utsuro_install_crash_handler(const char* log_path);
namespace utsuro::nt { void InitRegistry(const std::string&); }

namespace {

/// The title's executable, as its AppxManifest.xml names it (Application Executable="...").
std::string ExecutableFromManifest(const std::string& title_root) {
    std::ifstream f(title_root + "/AppxManifest.xml", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const size_t app = text.find("<Application ");
    if (app == std::string::npos) return {};
    const size_t at = text.find("Executable=\"", app);
    if (at == std::string::npos) return {};
    const size_t begin = at + 12, end = text.find('"', begin);
    return end == std::string::npos ? std::string() : text.substr(begin, end - begin);
}

} // namespace

extern "C" {

/// Claims the JIT region while a debugger is attached. True when executable memory is held.
bool utsuro_ios_prewarm_jit(bool self_only) { return utsuro::jit::Prewarm(self_only); }

/// The on-screen controller's state (Windows.Xbox.Input.GamepadButtons bits, sticks -1...1 with y up, triggers 0...1).
void utsuro_ios_set_pad(uint32_t buttons, float lx, float ly, float rx, float ry, float lt, float rt) {
    utsuro::input::PadState s;
    s.buttons = buttons;
    s.left_x = lx; s.left_y = ly; s.right_x = rx; s.right_y = ry;
    s.left_trigger = lt; s.right_trigger = rt;
    utsuro::input::SetTouchPad(s);
}

/// Boots the title in `dump` (a folder holding Mount/ and EmbeddedXvd/) and runs it until it exits. Blocks: call it from a
/// thread of its own. `layer` is the CAMetalLayer the frames are presented to.
int utsuro_ios_run(const char* dump, const char* data_dir, const char* crash_log, void* layer) {
    using namespace utsuro;
    if (data_dir != nullptr) setenv("UTSURO_DATA", data_dir, 1);
    if (crash_log != nullptr) utsuro_install_crash_handler(crash_log);
    nt::KernelOptions opt;
    opt.system_root = std::string(dump) + "/EmbeddedXvd/Windows";
    opt.title_root = std::string(dump) + "/Mount";
    opt.exe_name = ExecutableFromManifest(opt.title_root);
    if (opt.exe_name.empty()) opt.exe_name = "Minecraft.Client.exe";
    gpu::Init();
    gpu::SetLayer(layer);
    nt::Kernel kernel(std::move(opt));
    std::string error;
    if (!kernel.Boot(error)) {
        UT_ERROR("boot failed: %s", error.c_str());
        return 1;
    }
    return kernel.Run();
}

/// Saves the next presented frame as a PNG at `path`.
void utsuro_ios_screenshot(const char* path) {
    if (path != nullptr) utsuro::gpu::RequestSnapshot(path);
}

} // extern "C"
