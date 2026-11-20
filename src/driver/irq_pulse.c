#include "hw_map.h"
#include "driver_interface.h"
/*#include "perf_trace.h"*/

#include <errno.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/slog2.h>

static int     g_coid = -1;
static timer_t g_timer_id = -1;
static int     g_timer_active = 0;

int hw_irq_attach(int chid, int irq_num)
{
    struct sigevent ev;

    if (g_coid == -1) {
        g_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        if (g_coid == -1) {
            slog2f(NULL, 0, SLOG2_ERROR, "irq: ConnectAttach failed (errno=%d)", errno);
            return -1;
        }
    }

    SIGEV_PULSE_INIT(&ev, g_coid, SIGEV_PULSE_PRIO_INHERIT, SENSOR_PULSE_IRQ, 0);

    int iid = InterruptAttachEvent(irq_num, &ev, _NTO_INTR_FLAGS_TRK_MSK);
    if (iid == -1) {
        slog2f(NULL, 0, SLOG2_ERROR,
               "irq: InterruptAttachEvent(irq=%d) failed (errno=%d)", irq_num, errno);
        return -1;
    }

    slog2f(NULL, 0, SLOG2_INFO, "irq: attached IRQ %d -> pulse %d (iid=%d)",
           irq_num, SENSOR_PULSE_IRQ, iid);
    return iid;
}

void hw_irq_ack(int irq_num, int iid)
{
    /*TRACE_STAMP_IRQ_PULSE();*/
    hw_mmio_clear_irq();
    InterruptUnmask(irq_num, iid);
}

void hw_irq_detach(int iid)
{
    if (iid >= 0) {
        InterruptDetach(iid);
    }
}

int hw_timer_start(int chid, uint32_t rate_hz)
{
    struct sigevent   ev;
    struct itimerspec its;

    hw_timer_stop();

    if (rate_hz == 0 || rate_hz > SENSOR_MAX_RATE_HZ) {
        return 0;
    }

    if (g_coid == -1) {
        g_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        if (g_coid == -1) {
            slog2f(NULL, 0, SLOG2_ERROR, "timer: ConnectAttach failed (errno=%d)", errno);
            return -1;
        }
    }

    SIGEV_PULSE_INIT(&ev, g_coid, SIGEV_PULSE_PRIO_INHERIT, SENSOR_PULSE_TIMER, 0);

    if (timer_create(CLOCK_MONOTONIC, &ev, &g_timer_id) == -1) {
        slog2f(NULL, 0, SLOG2_ERROR, "timer: timer_create failed (errno=%d)", errno);
        return -1;
    }

    long period_ns = 1000000000L / (long)rate_hz;
    its.it_value.tv_sec     = period_ns / 1000000000L;
    its.it_value.tv_nsec    = period_ns % 1000000000L;
    its.it_interval.tv_sec  = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timer_settime(g_timer_id, 0, &its, NULL) == -1) {
        slog2f(NULL, 0, SLOG2_ERROR, "timer: timer_settime failed (errno=%d)", errno);
        timer_delete(g_timer_id);
        return -1;
    }

    g_timer_active = 1;
    slog2f(NULL, 0, SLOG2_INFO, "timer: started periodic pulse at %u Hz", rate_hz);
    return 0;
}

void hw_timer_stop(void)
{
    if (g_timer_active) {
        timer_delete(g_timer_id);
        g_timer_active = 0;
    }
}
