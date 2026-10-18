/*
 * husk-jit-arena: Husk's one executable region, handed out in 16 KiB pages to an engine that
 * frees and allocates again as it runs (the PS4 module: FEXCore's dispatcher and code buffer,
 * call trampolines, HLE veneers, shader walkers).
 *
 * The region is the one StikDebug grants at launch (or, on a Mac, a read+execute mapping with a
 * writable alias of the same pages). Every allocation has a writable address and an executable
 * one, and executable = writable + husk_jit_arena_delta() for all of them, so translating between
 * the two is one addition and a range check: async-signal-safe, no lock.
 *
 * Nothing here ever asks a debugger for memory. A request that would take the pages in use past
 * the ceiling given to open is refused (NULL), as is one the region cannot fit.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUSK_JIT_ARENA_PAGE ((size_t)16384)

/* Takes over [rw, rw+size) / [rx, rx+size), both 16 KiB aligned. ceiling_bytes caps the pages in
 * use at once (0: the whole region). Only one arena exists per process; opening again fails. */
bool husk_jit_arena_open_region(uint8_t *rw, uint8_t *rx, size_t size, size_t ceiling_bytes);

/* Finds the platform's region and opens the arena on it: on the phone, the region StikDebug
 * granted at launch (husk_ios_jit_get_mapping); on a Mac, a new read+execute mapping with a
 * vm_remap'd writable alias, region_bytes long (TL_XMEM_MIB overrides it). */
bool husk_jit_arena_open(size_t region_bytes, size_t ceiling_bytes, char *err, size_t errlen);

bool husk_jit_arena_is_open(void);

/* bytes is rounded up to whole pages. Returns the writable address and stores the executable
 * one in *rx_out; NULL when refused. Freed pages are zeroed (UDF), so a stale branch into them
 * stops with SIGILL instead of running leftover code. */
void *husk_jit_arena_alloc(size_t bytes, void **rx_out);
/* rw and bytes as allocated. Returns false (and frees nothing) for a range that is not exactly
 * pages this arena handed out. */
bool husk_jit_arena_free(void *rw, size_t bytes);

/* rx - rw, the same for every allocation. */
intptr_t husk_jit_arena_delta(void);
/* Translation with a range check: the other view of p, or NULL when p is not in the region. */
void *husk_jit_arena_rx_of(const void *rw);
void *husk_jit_arena_rw_of(const void *rx);

uint8_t *husk_jit_arena_rw_base(void);      /* the region's writable start */
size_t husk_jit_arena_held_bytes(void);    /* the region's size */
size_t husk_jit_arena_ceiling_bytes(void);
size_t husk_jit_arena_used_bytes(void);
size_t husk_jit_arena_high_water_bytes(void);
size_t husk_jit_arena_refusals(void);

#ifdef __cplusplus
}
#endif
