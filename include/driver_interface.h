/*
 * driver_interface.h  --  THE contract between driver, clients, tests, demo.
 * Owner: Siddhant.
 *
 * STATUS: FROZEN v1 (2026-11-18).
 *   Do not change devctl codes, pulse codes or struct layouts.
 *   Any change = bump SENSOR_IFACE_VERSION and tell Siddhant first.
 *
 * Everyone codes against this file: Aeknath (pulse), Basith (MMIO),
 * Zaid (concurrency/notify), Lidiya (security/benchmarks), Benson (.build).
 */
#ifndef DRIVER_INTERFACE_H
#define DRIVER_INTERFACE_H

#include <stdint.h>
#include <devctl.h>
#include <sys/neutrino.h>   /* _PULSE_CODE_MINAVAIL */

#define SENSOR_IFACE_VERSION   1

/* ---- Device path ---------------------------------------------------- */
#define SENSOR_DEV_PATH        "/dev/sensor0"

/* ---- Data types (fixed width, no padding surprises) ----------------- */
typedef struct {
    uint64_t timestamp_ns;   /* CLOCK_MONOTONIC at sample time          */
    uint32_t seq;            /* increments per sample                   */
    uint32_t value;          /* raw sensor/register value               */
} sensor_sample_t;           /* 16 bytes; read() returns exactly one    */

typedef struct {
    uint32_t irq_count;      /* interrupts (pulses) handled             */
    uint32_t overruns;       /* samples dropped because the buffer was full */
    uint32_t rate_hz;        /* current sample rate (0 = on demand)     */
    uint32_t iface_version;  /* SENSOR_IFACE_VERSION                    */
} sensor_status_t;

/* ---- devctl() commands ---------------------------------------------
 * __DIOF  = data flows driver -> client
 * __DIOT  = data flows client -> driver
 * __DION  = no data
 */
#define DCMD_SENSOR_GET_SAMPLE  __DIOF(_DCMD_MISC, 0x01, sensor_sample_t)   /* latest value, never blocks */
#define DCMD_SENSOR_GET_STATUS  __DIOF(_DCMD_MISC, 0x02, sensor_status_t)
#define DCMD_SENSOR_SET_RATE    __DIOT(_DCMD_MISC, 0x03, uint32_t)          /* Hz, 0..SENSOR_MAX_RATE_HZ */
#define DCMD_SENSOR_RESET       __DION(_DCMD_MISC, 0x04)                    /* zero counters, empty buffer */

#define SENSOR_MAX_RATE_HZ      10000u

/* ---- read() semantics -----------------------------------------------
 * - read(fd, buf, n) needs n >= sizeof(sensor_sample_t), else EINVAL.
 * - Returns exactly ONE queued sample (oldest first).
 * - Buffer empty + blocking fd  -> read() waits until a sample arrives.
 * - Buffer empty + O_NONBLOCK   -> read() fails with EAGAIN.
 * - Buffer holds SENSOR_RING_SIZE samples; when full the OLDEST is
 *   dropped and status.overruns is incremented.
 * - poll()/select() report "readable" whenever the buffer is non-empty.
 */
#define SENSOR_RING_SIZE        64

/* ---- write() semantics ---------------------------------------------
 * write(fd, &uint32_t, 4): inject a value as if the sensor produced it.
 * It becomes a queued sample (wakes blocked readers / poll()).
 * Used on QEMU where there is no real sensor. Any other size -> EINVAL.
 */

/* ---- Error codes -----------------------------------------------------
 * EINVAL : bad size or out-of-range argument
 * EAGAIN : non-blocking read with no data (or too many waiting readers)
 * ENOSYS : unknown devctl command
 */

/* ---- Pulse codes (driver-internal, Aeknath) ------------------------- */
#define SENSOR_PULSE_IRQ        (_PULSE_CODE_MINAVAIL + 1)   /* hardware IRQ fired     */
#define SENSOR_PULSE_TIMER      (_PULSE_CODE_MINAVAIL + 2)   /* simulated-sample tick  */

#endif /* DRIVER_INTERFACE_H */
