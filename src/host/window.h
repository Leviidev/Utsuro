// window.h -- the host window (Cocoa) the title's frames appear in.
#pragma once

#include <functional>

namespace utsuro::host {

/// Opens the window and runs the event loop on the calling (main) thread; `run_title` is started on
/// another thread and the loop ends when it returns. Returns its result.
int RunWindowed(std::function<int()> run_title);

} // namespace utsuro::host
