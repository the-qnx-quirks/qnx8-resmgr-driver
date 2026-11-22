/*
 * resmgr.c -- POSIX resource manager for /dev/sensor0 (Siddhant)
 *
 * Build (QNX SDP 8.0, after sourcing qnxsdp-env):
 *   qcc -V gcc_ntox86_64 -Wall -Iinclude -Isrc/driver -DUSE_SECURITY_INIT \
 *       -o sensor_resmgr src/driver/resmgr.c src/driver/concurrency.c \
 *       src/driver/security_init.c src/driver/main_stub.c
 *   (use gcc_ntoaarch64le for Raspberry Pi 4/5)
 *   -DUSE_SECURITY_INIT  turns on Lidiya's lockdown + root drop
 *   -DUSE_THREAD_POOL    serves clients with Zaid's thread pool instead of 1 thread
 *
 * Startup order in sensor_resmgr_init():
 *   1. create /dev/sensor0 + pulse   (needs path-space ability - still available
 * before security_init_hw_privileges() runs)
 *   2. security_init_hw_privileges() (locks four required abilities:
 * IO, INTERRUPT, MEM_PHYS, PUBLIC_CHANNEL, plus SETUID/SETGID for root drop)
 *   3. sensor_hw_init()              (Basith mmap_device_memory, Aeknath InterruptAttachEvent)
 *   4. security_drop_root()          (drops from UID 0 to non-root UID/GID)
 *
 * Important: resmgr_attach() must be called before security_init_hw_privileges()
 * so that the path-space ability (PROCMGR_AID_PATHSPACE) is still available.
 * After security_init_hw_privileges() runs, the ability mask is permanently
 * locked via PROCMGR_AOP_LOCK, and no further ability modifications are possible.
 *
 * Hooks for teammates are marked  >>> HOOK <<<
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "driver_interface.h"
#include "resmgr.h"
#include "concurrency.h"        /* Zaid: thread pool + notify helpers */
#include "security_init.h"      /* Lidiya: security_init_hw_privileges(), security_drop_root() */
#include <grp.h>                /* getgrnam() for SENSOR_CLIENT_GID fallback */
#ifdef USE_SECURITY_INIT
#include "security_init.h"      /* Lidiya: security_init_hw_privileges(), security_drop_root() */
#endif

#define MAX_WAITERS 16

/* ---- Driver state (all protected by g_lock) ------------------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sensor_status_t g_status = { 0, 0, 0, SENSOR_IFACE_VERSION };
static sensor_sample_t g_ring[SENSOR_RING_SIZE];
static unsigned        g_head, g_count;
static uint32_t        g_seq;
static sensor_sample_t g_latest;
static int             g_have_latest;
static uint32_t        g_last_value;               /* simulated register */
static struct { int active; int rcvid; } g_waiters[MAX_WAITERS];
static iofunc_notify_t g_notify[3];                /* poll()/select() clients */

/* >>> HOOK (Basith): replace with in32() on the mapped register <<< */
static uint32_t hw_read_register(void)
{
    return g_last_value;
}

/* >>> HOOK (Basith + Aeknath): privileged hardware setup goes here <<<
 * Runs AFTER security_init_hw_privileges() and BEFORE security_drop_root(),
 * i.e. exactly when mmap_device_memory() and InterruptAttachEvent() are allowed.
 * Return 0 on success, -1 on failure. */
static int sensor_hw_init(void)
{
    return 0;
}

static void make_sample(sensor_sample_t *s, uint32_t value)   /* caller holds g_lock */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    s->seq   = ++g_seq;
    s->value = value;
}

/* ---- Producer entry point (see resmgr.h) ---------------------------- */
void sensor_data_ready(uint32_t value)
{
    sensor_sample_t s;
    int i;

    pthread_mutex_lock(&g_lock);
    g_last_value = value;
    make_sample(&s, value);
    g_latest = s;
    g_have_latest = 1;

    /* A reader is already waiting (the buffer was empty): hand it over directly. */
    for (i = 0; i < MAX_WAITERS; i++) {
        if (g_waiters[i].active) {
            g_waiters[i].active = 0;
            MsgReply(g_waiters[i].rcvid, (int)sizeof(s), &s, sizeof(s));
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }

    if (g_count == SENSOR_RING_SIZE) {             /* full: drop the oldest */
        g_head = (g_head + 1) % SENSOR_RING_SIZE;
        g_count--;
        g_status.overruns++;
    }
    g_ring[(g_head + g_count) % SENSOR_RING_SIZE] = s;
    g_count++;

    /* Wake poll()/select() clients (Zaid's helper in concurrency.c). */
    sensor_notify_trigger(g_notify);
    pthread_mutex_unlock(&g_lock);
}

/* >>> HOOK (Aeknath): the pulse from the real interrupt arrives here <<< */
static int irq_pulse_handler(message_context_t *ctp, int code,
                             unsigned flags, void *handle)
{
    (void)ctp; (void)code; (void)flags; (void)handle;
    pthread_mutex_lock(&g_lock);
    g_status.irq_count++;
    pthread_mutex_unlock(&g_lock);
    sensor_data_ready(hw_read_register());
    /* InterruptUnmask() goes here once real IRQs are attached. */
    return 0;
}

/* ---- Resource manager handlers -------------------------------------- */
static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          attr;

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    sensor_sample_t s;
    int i;
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (msg->i.nbytes < sizeof(s)) return EINVAL;  /* buffer too small for one sample */

    pthread_mutex_lock(&g_lock);
    if (g_count > 0) {                             /* data ready: return oldest */
        s = g_ring[g_head];
        g_head = (g_head + 1) % SENSOR_RING_SIZE;
        g_count--;
        pthread_mutex_unlock(&g_lock);
        MsgReply(ctp->rcvid, (int)sizeof(s), &s, sizeof(s));
        return _RESMGR_NOREPLY;
    }
    if (ocb->ioflag & O_NONBLOCK) {                /* empty + non-blocking */
        pthread_mutex_unlock(&g_lock);
        return EAGAIN;
    }
    for (i = 0; i < MAX_WAITERS; i++) {            /* empty + blocking: wait */
        if (!g_waiters[i].active) {
            g_waiters[i].active = 1;
            g_waiters[i].rcvid  = ctp->rcvid;
            pthread_mutex_unlock(&g_lock);
            return _RESMGR_NOREPLY;                /* answered later by sensor_data_ready() */
        }
    }
    pthread_mutex_unlock(&g_lock);
    return EAGAIN;                                 /* too many waiting readers */
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb)
{
    uint32_t v;
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (msg->i.nbytes != sizeof(v)) return EINVAL;
    if (resmgr_msgread(ctp, &v, sizeof(v), sizeof(msg->i)) != (int)sizeof(v)) return EIO;

    sensor_data_ready(v);                          /* injected value becomes a sample */
    _IO_SET_WRITE_NBYTES(ctp, sizeof(v));
    return _RESMGR_NPARTS(0);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    void *data   = _DEVCTL_DATA(msg->i);
    int   nbytes = 0;

    switch (msg->i.dcmd) {
    case DCMD_SENSOR_GET_SAMPLE:
        if (msg->i.nbytes < (int)sizeof(sensor_sample_t)) return EINVAL;
        pthread_mutex_lock(&g_lock);
        if (g_have_latest) *(sensor_sample_t *)data = g_latest;
        else make_sample((sensor_sample_t *)data, hw_read_register());
        pthread_mutex_unlock(&g_lock);
        nbytes = sizeof(sensor_sample_t);
        break;

    case DCMD_SENSOR_GET_STATUS:
        if (msg->i.nbytes < (int)sizeof(sensor_status_t)) return EINVAL;
        pthread_mutex_lock(&g_lock);
        *(sensor_status_t *)data = g_status;
        pthread_mutex_unlock(&g_lock);
        nbytes = sizeof(sensor_status_t);
        break;

    case DCMD_SENSOR_SET_RATE: {
        if (msg->i.nbytes < (int)sizeof(uint32_t)) return EINVAL;
        uint32_t hz = *(uint32_t *)data;
        if (hz > SENSOR_MAX_RATE_HZ) return EINVAL;        /* out of range */
        pthread_mutex_lock(&g_lock);
        g_status.rate_hz = hz;       /* >>> HOOK (Aeknath): re-arm timer/IRQ rate <<< */
        pthread_mutex_unlock(&g_lock);
        break;
    }

    case DCMD_SENSOR_RESET:
        pthread_mutex_lock(&g_lock);
        g_seq = 0; g_head = 0; g_count = 0; g_have_latest = 0;
        g_status.irq_count = 0; g_status.overruns = 0;
        pthread_mutex_unlock(&g_lock);
        break;

    default:
        return ENOSYS;
    }

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    msg->o.nbytes  = nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

/* poll()/select() support (Zaid builds the thread pool on top of this) */
static int io_notify(resmgr_context_t *ctp, io_notify_t *msg, iofunc_ocb_t *ocb)
{
    int trig = _NOTIFY_COND_OUTPUT;                /* always writable */
    (void)ocb;
    pthread_mutex_lock(&g_lock);
    if (g_count > 0) trig |= _NOTIFY_COND_INPUT;   /* readable when data is queued */
    pthread_mutex_unlock(&g_lock);
    return iofunc_notify(ctp, msg, g_notify, trig, NULL, NULL);
}

/* A client gave up (signal/timeout) while waiting in a blocking read. */
static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, iofunc_ocb_t *ocb)
{
    int i;
    int status = iofunc_unblock_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_WAITERS; i++) {
        if (g_waiters[i].active && g_waiters[i].rcvid == ctp->rcvid) {
            g_waiters[i].active = 0;
            pthread_mutex_unlock(&g_lock);
            return EINTR;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return _RESMGR_NOREPLY;
}

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
{
    iofunc_notify_remove(ctp, g_notify);
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

/* ---- Public entry points -------------------------------------------- */
dispatch_t *sensor_resmgr_init(void)
{
    dispatch_t   *dpp;
    resmgr_attr_t rattr;

    if ((dpp = dispatch_create()) == NULL) {
        perror("dispatch_create");
        return NULL;
    }

    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS,      &io_funcs);
    io_funcs.read      = io_read;
    io_funcs.write     = io_write;
    io_funcs.devctl    = io_devctl;
    io_funcs.notify    = io_notify;
    io_funcs.unblock   = io_unblock;
    io_funcs.close_ocb = io_close_ocb;

    iofunc_attr_init(&attr, S_IFCHR | 0660, NULL, NULL);
    attr.uid = 0;            /* driver runs as root */
    attr.gid = SENSOR_CLIENT_GID;  /* group ownership for client access */
    sensor_notify_init(g_notify);

    if (resmgr_attach(dpp, &rattr, SENSOR_DEV_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach");
        return NULL;
    }
    if (pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, SENSOR_PULSE_IRQ,
                     irq_pulse_handler, NULL) == -1) {
        perror("pulse_attach");
        return NULL;
    }

    /* /dev/sensor0 now exists. From here on we no longer need the
     * path-space ability, so Lidiya's lockdown can remove it. */
#ifdef USE_SECURITY_INIT
    if (security_init_hw_privileges() != 0) {
        perror("security_init_hw_privileges");
        return NULL;
    }
#endif

    if (sensor_hw_init() != 0) {                   /* Basith / Aeknath */
        fprintf(stderr, "sensor_hw_init failed\n");
        return NULL;
    }

#ifdef USE_SECURITY_INIT
    if (security_drop_root(SENSOR_DRIVER_UID, SENSOR_CLIENT_GID) != 0) {
        perror("security_drop_root");
        return NULL;
    }
#endif
    return dpp;
}

int sensor_resmgr_run(dispatch_t *dpp)
{
#ifdef USE_THREAD_POOL
    return sensor_concurrency_run(dpp);            /* Zaid: thread pool, blocks forever */
#else
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) { perror("dispatch_context_alloc"); return -1; }

    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return -1;
        }
        dispatch_handler(ctp);
    }
#endif
}
