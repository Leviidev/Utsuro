// hle_classes.h -- guest DLL exports that the host answers with COM objects of its own (see hle.cpp for the hooking).
#pragma once

#include "nt/kernel.h"

namespace utsuro::nt::mmdevice {
/// MMDevAPI.dll!DllGetClassObject.
uint64_t DllGetClassObject(Kernel& kernel, NtThread& thread, SyscallArgs& args);
/// MMDevAPI.dll!ActivateAudioInterfaceAsync: an IAudioClient for any device path, the completion handler called at once.
uint64_t ActivateAudioInterfaceAsync(Kernel& kernel, NtThread& thread, SyscallArgs& args);
}
