/* resmgr.h -- public entry points of the /dev/sensor0 resource manager (Siddhant) */
#ifndef RESMGR_H
#define RESMGR_H

#include <stdint.h>
#include <sys/dispatch.h>

/* Create /dev/sensor0 and the IRQ pulse, then (with -DUSE_SECURITY_INIT):
 * security_init_hw_privileges() -> sensor_hw_init() -> security_drop_root().
 * Returns NULL on failure. */
dispatch_t *sensor_resmgr_init(void);

/* Serve requests forever: one thread by default, Zaid's thread pool when
 * built with -DUSE_THREAD_POOL. Returns -1 on error. */
int sensor_resmgr_run(dispatch_t *dpp);

/* THE producer entry point. Call it whenever a new sensor value exists:
 *   - Aeknath's IRQ handler (after reading the register)
 *   - Basith's MMIO polling code
 *   - write() injection on QEMU
 * It timestamps the value, queues it (or hands it straight to a blocked
 * reader) and triggers iofunc_notify so poll()/select() wake up.
 * Safe to call from any thread. */
void sensor_data_ready(uint32_t value);

#endif
