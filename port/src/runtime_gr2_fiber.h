/* sceFiber on hosts whose guest CPU is not the host's (runtime_gr2_fiber.c); x86-64 Linux and
 * Windows switch host contexts instead (runtime_gr2.c). Tests define GR2_FIBER_GUEST_CONTEXT to
 * build this implementation on any host. */
#ifndef TLG_RUNTIME_GR2_FIBER_H
#define TLG_RUNTIME_GR2_FIBER_H
#include <stdint.h>
#include "guest_cpu.h"

#if !defined(GR2_FIBER_GUEST_CONTEXT) && !defined(_WIN32) && !defined(GUEST_CPU_NATIVE)
#define GR2_FIBER_GUEST_CONTEXT 1
#endif

#define FIBER_MAGIC UINT64_C(0x5245424946325247) /* "GR2FIBER" */
#define FIBER_ERR_NULL ((int32_t)0x80590001)
#define FIBER_ERR_INVALID ((int32_t)0x80590004)
#define FIBER_ERR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERR_STATE ((int32_t)0x80590006)

#if defined(GR2_FIBER_GUEST_CONTEXT)
/* The exports' contracts (runtime_gr2.c's table); `entry` is the guest's
 * void entry(uint64_t arg_on_initialize, uint64_t arg_on_run). */
int32_t gr2_fiber_initialize(void *guest, const char *name, uintptr_t entry, uint64_t arg_on_initialize,
                             void *context, uint64_t context_size, const void *option, uint32_t build_version);
int32_t gr2_fiber_finalize(void *guest);
int32_t gr2_fiber_run(void *guest, uint64_t arg_on_run_to, uint64_t *arg_on_return);
int32_t gr2_fiber_switch(void *guest, uint64_t arg_on_run_to, uint64_t *arg_on_run);
int32_t gr2_fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run);
#endif
#endif
