// The C side the app talks to: the JIT region and the runtime's entry points (src/jit, src/host/ios_entry.mm), and the on-device
// pairing library (libutsuro_rppairing.a, idevice's PairableHost).
#ifndef UTSURO_BRIDGE_H
#define UTSURO_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "UtsuroRPPairing.h"

/* src/jit/husk-ios-jit.h */
void husk_ios_jit_install_trap_handler(void);
bool husk_ios_jit_is_available(void);
void husk_ios_jit_detach(void);
const char *husk_ios_jit_self_route(void);
void husk_ios_jit_log_footprint(const char *tag);
size_t husk_ios_available_memory(void);

/* src/host/ios_entry.mm */
bool utsuro_ios_prewarm_jit(bool self_only);
void utsuro_ios_set_pad(uint32_t buttons, float lx, float ly, float rx, float ry, float lt, float rt);
void utsuro_ios_screenshot(const char *path);
int utsuro_ios_run(const char *dump, const char *data_dir, const char *crash_log, void *layer);

#endif
