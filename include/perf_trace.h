/*=============================================================================
 *  perf_trace.h - Microsecond Latency Instrumentation
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#ifndef PERF_TRACE_H_INCLUDED
#define PERF_TRACE_H_INCLUDED

#include <sys/neutrino.h>
#include <sys/syspage.h>    /* for SYSPAGE_ENTRY(qtime) */
#include <sys/trace.h>      /* for TraceEvent(), _NTO_TRACE_INSERTUSRSTREVENT_CLASS, _SUBSCRIPTION_CLASS */
#include <sys/slog2.h>      /* for slog2f */
#include <stdint.h>

/*
 * Safe no-op fallback: if tracing is not enabled (TRACE_DISABLED=1),
 * these macros expand to nothing. The build system can define
 * TRACE_DISABLED=1 to disable tracing completely; otherwise TraceEvent
 * calls are harmless no-ops at the kernel level if no subscriber exists.
 */
#ifndef TRACE_DISABLED
#define TRACE_DISABLED 0
#endif

/*
 * TRACE_STAMP_ISR - Record an ISR (interrupt service routine) entry/exit event.
 * Use: TRACE_STAMP_ISR(seq_id)
 *      seq_id: unique sequence identifier for the ISR
 * Context: IRQ pulse / interrupt tracking
 */
#define TRACE_STAMP_ISR(seq_id) do { \
    if (!(TRACE_DISABLED)) { \
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, \
            _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS), \
            (uint16_t)0x01,           /* subclass: ISR */ \
            (uint64_t)(seq_id),      /* arg0 = seq_id */ \
            (uint64_t)ClockCycles(), /* arg1 = timestamp */ \
            (uint64_t)__LINE__,      /* arg2 = source line */ \
            (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file */ \
    } \
} while(0)

/*
 * TRACE_STAMP_PULSE - Record a user-space pulse wakeup event.
 * Use: TRACE_STAMP_PULSE(seq_id)
 *      seq_id: unique sequence identifier for the pulse
 * Context: Aeknath's pulse-driven IRQ wakeup path
 */
#define TRACE_STAMP_PULSE(seq_id) do { \
    if (!(TRACE_DISABLED)) { \
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, \
            _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS), \
            (uint16_t)0x02,           /* subclass: PULSE */ \
            (uint64_t)(seq_id),      /* arg0 = seq_id */ \
            (uint64_t)ClockCycles(), /* arg1 = timestamp */ \
            (uint64_t)__LINE__,      /* arg2 = source line */ \
            (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file */ \
    } \
} while(0)

/*
 * TRACE_STAMP_REPLY - Record a message reply / IPC handoff event.
 * Use: TRACE_STAMP_REPLY(seq_id)
 *      seq_id: unique sequence identifier for the reply
 * Context: Zaid's iofunc_notify / thread pool handoff
 */
#define TRACE_STAMP_REPLY(seq_id) do { \
    if (!(TRACE_DISABLED)) { \
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, \
            _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS), \
            (uint16_t)0x03,           /* subclass: REPLY */ \
            (uint64_t)(seq_id),      /* arg0 = seq_id */ \
            (uint64_t)ClockCycles(), /* arg1 = timestamp */ \
            (uint64_t)__LINE__,      /* arg2 = source line */ \
            (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file */ \
    } \
} while(0)

/*
 * TRACE_STAMP_MMIO_DONE - Record MMIO completion event with register value.
 * Use: TRACE_STAMP_MMIO_DONE(seq_id, reg_val)
 *      seq_id: unique sequence identifier for the MMIO operation
 *      reg_val: the register value read/written
 * Context: Basith's user-space MMIO/DMA completion
 */
#define TRACE_STAMP_MMIO_DONE(seq_id, reg_val) do { \
    if (!(TRACE_DISABLED)) { \
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, \
            _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS), \
            (uint16_t)0x04,           /* subclass: MMIO_DONE */ \
            (uint64_t)(seq_id),      /* arg0 = seq_id */ \
            (uint64_t)(reg_val),     /* arg1 = register value */ \
            (uint64_t)ClockCycles(), /* arg2 = timestamp */ \
            (uint64_t)__LINE__,      /* arg3 = source line */ \
            (uint64_t)(uintptr_t)__FILE__); /* arg4 = source file */ \
    } \
} while(0)

/*
 * TRACE_STAMP_CLIENT_DELIVERED - Record client data delivery handoff.
 * Use: TRACE_STAMP_CLIENT_DELIVERED(seq_id)
 *      seq_id: unique sequence identifier for the client delivery
 * Context: Zaid's iofunc_notify / I/O pipeline handoff
 */
#define TRACE_STAMP_CLIENT_DELIVERED(seq_id) do { \
    if (!(TRACE_DISABLED)) { \
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, \
            _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS), \
            (uint16_t)0x05,           /* subclass: CLIENT_DELIVERED */ \
            (uint64_t)(seq_id),      /* arg0 = seq_id */ \
            (uint64_t)ClockCycles(), /* arg1 = timestamp */ \
            (uint64_t)__LINE__,      /* arg2 = source line */ \
            (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file */ \
    } \
} while(0)

/*
 * cycles_to_ns - Convert clock cycle delta to nanoseconds.
 * Uses the global cycles_per_sec from the QNX syspage (qtime).
 * Performance: zero-overhead inline, evaluated at link time against
 *   the running kernel. QNX SDP 8.0 constant.
 *
 * Use: uint64_t ns = cycles_to_ns(delta_cycles);
 */
static inline uint64_t cycles_to_ns(uint64_t delta_cycles)
{
    /* Get cycles_per_sec from the system page. */
    uint64_t cycles_per_sec = (uint64_t)
        SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    if (cycles_per_sec == 0) {
        return 0; /* Avoid division by zero */
    }

    /* ns = (delta_cycles * 1_000_000_000) / cycles_per_sec */
    return (delta_cycles * 1000000000ULL) / cycles_per_sec;
}

/*=============================================================================
 * Internal trace event definitions (usrapp tracing)
 * These map to _NTO_TRACE_INSERTUSRSTREVENT with subclass codes.
 *===========================================================================*/

/* IRQ pulse subclass code (user-defined but documented) */
#define USRSTREVENT_SUBCLASS_IRQ_PULSE  ((_USRSTREVENT_SUBCLASS_T)0x01)

/* MMIO done subclass code */
#define USRSTREVENT_SUBCLASS_MMIO_DONE   ((_USRSTREVENT_SUBCLASS_T)0x02)

/* Client delivered subclass code */
#define USRSTREVENT_SUBCLASS_CLIENT_DELIVERED  ((_USRSTREVENT_SUBCLASS_T)0x03)

/* ISR subclass code */
#define USRSTREVENT_SUBCLASS_ISR  ((_USRSTREVENT_SUBCLASS_T)0x01)

/* Pulse subclass code */
#define USRSTREVENT_SUBCLASS_PULSE  ((_USRSTREVENT_SUBCLASS_T)0x02)

/* Reply subclass code */
#define USRSTREVENT_SUBCLASS_REPLY  ((_USRSTREVENT_SUBCLASS_T)0x03)

/* MMIO done subclass code (duplicate for convenience) */
#define USRSTREVENT_SUBCLASS_MMIO_DONE2   ((_USRSTREVENT_SUBCLASS_T)0x04)

/* Client delivered subclass code (duplicate for convenience) */
#define USRSTREVENT_SUBCLASS_CLIENT_DELIVERED2  ((_USRSTREVENT_SUBCLASS_T)0x05)

/*
 * traceusrinfopulse - Internal helper: emit a pulse-style usrstrevent.
 * Maps to TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, ...)
 *  - class: _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS)
 *  - subclass: USRSTREVENT_SUBCLASS_IRQ_PULSE
 *  - payload: seq_id (uint64_t), cycles (uint64_t), line (int), file (const char*)
 *
 * NOTE: The name "traceusrinfopulse_" from the original file has been
 *       renamed to avoid confusion; use the TRACE_STAMP_PULSE() macro instead.
 */
static inline void traceusrinfopulse(uint16_t seq_id, uint64_t cycles)
{
    /* Backward-compatibility helper — maps to the original pattern. */
    int retval = TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT,
        _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS),
        USRSTREVENT_SUBCLASS_IRQ_PULSE,
        (uint64_t)seq_id,          /* arg0 = seq_id */
        cycles,                    /* arg1 = timestamp_cycles */
        (uint64_t)__LINE__,        /* arg2 = source line */
        (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file ptr */
    (void)retval;
}

/*
 * traceusrstremmio - Internal helper: emit an MMIO usrstrevent.
 * Maps to TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, ...)
 *  - class: _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS)
 *  - subclass: USRSTREVENT_SUBCLASS_MMIO_DONE
 *  - payload: seq_id (uint64_t), reg_val (uint64_t), cycles (uint64_t), line (int), file (const char*)
 */
static inline void traceusrstremmio(uint16_t seq_id, uint64_t reg_val)
{
    int retval = TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT,
        _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS),
        USRSTREVENT_SUBCLASS_MMIO_DONE,
        (uint64_t)seq_id,          /* arg0 = seq_id */
        reg_val,                   /* arg1 = register value */
        (uint64_t)ClockCycles(),   /* arg2 = timestamp_cycles */
        (uint64_t)__LINE__,        /* arg3 = source line */
        (uint64_t)(uintptr_t)__FILE__); /* arg4 = source file ptr */
    (void)retval;
}

/*
 * traceusrstrecv - Internal helper: emit a client-delivered usrstrevent.
 * Maps to TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, ...)
 *  - class: _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS)
 *  - subclass: USRSTREVENT_SUBCLASS_CLIENT_DELIVERED
 *  - payload: seq_id (uint64_t), cycles (uint64_t), line (int), file (const char*)
 */
static inline void traceusrstrecv(uint16_t seq_id, uint64_t cycles)
{
    int retval = TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT,
        _SUBSCRIPTION_CLASS(_NTO_TRACE_USRSTREVENT_CLASS),
        USRSTREVENT_SUBCLASS_CLIENT_DELIVERED,
        (uint64_t)seq_id,          /* arg0 = seq_id */
        cycles,                    /* arg1 = timestamp_cycles */
        (uint64_t)__LINE__,        /* arg2 = source line */
        (uint64_t)(uintptr_t)__FILE__); /* arg3 = source file ptr */
    (void)retval;
}

#endif /* !defined(PERF_TRACE_H_INCLUDED) */