/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * On-device remote pairing (iOS 27 and later): src/rppairing-ios, linked as
 * libhusk_rppairing.a by scripts/build_rppairing_ios.sh. Keep in step with
 * src/rppairing-ios/src/lib.rs.
 */
#ifndef HUSK_RPPAIRING_H
#define HUSK_RPPAIRING_H

#include <stddef.h>
#include <stdint.h>

typedef struct HuskRPPairing HuskRPPairing;
typedef void (*HuskRPPairingPinCallback)(const char *pin, void *context);

/* New host identity and a listener on all IPv4 interfaces. NULL on failure,
   with *error set (free with husk_rppairing_string_free). */
HuskRPPairing *husk_rppairing_new(const char *name, char **error);
uint16_t husk_rppairing_port(const HuskRPPairing *session);
/* The Bonjour instance name to publish; owned by the session. */
const char *husk_rppairing_service_name(const HuskRPPairing *session);
size_t husk_rppairing_txt_count(const HuskRPPairing *session);
const char *husk_rppairing_txt_key(const HuskRPPairing *session, size_t index);
const char *husk_rppairing_txt_value(const HuskRPPairing *session, size_t index);
/* Blocks until paired (0), failed (1, *error set) or cancelled (2). On success
   *out_plist and *out_len hold the RPPairing plist (husk_rppairing_bytes_free)
   and *out_device_name the device's name (husk_rppairing_string_free). */
int32_t husk_rppairing_accept(const HuskRPPairing *session,
                              HuskRPPairingPinCallback pin_callback, void *pin_context,
                              uint8_t **out_plist, size_t *out_len,
                              char **out_device_name, char **error);
/* Ends a running or future accept with 2. Any thread. */
void husk_rppairing_cancel(const HuskRPPairing *session);
/* Only after accept has returned (or was never called). */
void husk_rppairing_free(HuskRPPairing *session);
void husk_rppairing_bytes_free(uint8_t *bytes, size_t len);
void husk_rppairing_string_free(char *string);

#endif /* HUSK_RPPAIRING_H */
