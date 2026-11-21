/*=============================================================================
 *  security_init.c - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#define _GNU_SOURCE
#include "security_init.h"
#include <stdint.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/slog2.h>
#include <sys/procmgr.h>

/*
 * Static state so we only do the security init once per process lifetime.
 * After security_init() runs, the microkernel abilities are locked and
 * the process has dropped to non-root UID/GID.
 */
static int g_security_init_done = 0;

/*
 * security_init_hw_privileges - Deny all non-essential microkernel abilities
 * and permanently lock the four required ones (IO, INTERRUPT, MEM_PHYS, PUBLIC_CHANNEL).
 * Must be called before security_drop_root().
 *
 * QNX SDP 8.0 Ability Domain Strategy (single procmgr_ability() call with tuples):
 *   - Each tuple is: (domain-flags | operation-flags | ability-ID)
 *   - Tuples are separated by their operation/ability arguments
 *   - The list is terminated with PROCMGR_AID_EOL
 *   - PROCMGR_AOP_SUBRANGE with (uint64_t) min/max is used for SETUID/SETGID
 *     to scope them ONLY to PROCMGR_ADN_ROOT (keeps them denied in NONROOT
 *     so the process cannot re-escalate after dropping root).
 *
 * Tuple order in the single procmgr_ability() call:
 *   1. Allow MEM_PHYS in both domains
 *   2. Allow INTERRUPT in both domains
 *   3. Allow IO in both domains
 *   4. Allow PUBLIC_CHANNEL in both domains
 *   5. Allow SETUID only in ROOT domain, with subrange [SENSOR_DRIVER_UID, SENSOR_DRIVER_UID]
 *   6. Allow SETGID only in ROOT domain, with subrange [SENSOR_CLIENT_GID, SENSOR_CLIENT_GID]
 *   7. Deny ALL remaining abilities in BOTH domains, terminating with PROCMGR_AID_EOL
 *
 * Critical: SETUID/SETGID are allowed only in PROCMGR_ADN_ROOT with subrange
 * matching the target UID/GID. In PROCMGR_ADN_NONROOT they are automatically
 * denied and permanently locked after the root drop, preventing re-escalation.
 *
 * Returns 0 on success, -1 on error with errno set.
 */
int security_init_hw_privileges(void)
{
    int status;

    if (g_security_init_done) {
        errno = EALREADY;
        return -1;
    }

    /* Single procmgr_ability() call with 7 tuples terminated by PROCMGR_AID_EOL:
     * Tuple 1: Allow MEM_PHYS in both domains */
    /* Tuple 2: Allow INTERRUPT in both domains */
    /* Tuple 3: Allow IO in both domains */
    /* Tuple 4: Allow PUBLIC_CHANNEL in both domains */
    /* Tuple 5: Allow SETUID only in ROOT domain, subrange [SENSOR_DRIVER_UID, SENSOR_DRIVER_UID] */
    /* Tuple 6: Allow SETGID only in ROOT domain, subrange [SENSOR_CLIENT_GID, SENSOR_CLIENT_GID] */
    /* Tuple 7: Deny ALL remaining abilities in BOTH domains, terminate with PROCMGR_AID_EOL */

    status = procmgr_ability(
        0,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_MEM_PHYS,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_INTERRUPT,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_IO,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PUBLIC_CHANNEL,
        PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_SUBRANGE | PROCMGR_AOP_LOCK | PROCMGR_AID_SETUID,
        (uint64_t)SENSOR_DRIVER_UID, (uint64_t)SENSOR_DRIVER_UID,
        PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_SUBRANGE | PROCMGR_AOP_LOCK | PROCMGR_AID_SETGID,
        (uint64_t)SENSOR_CLIENT_GID, (uint64_t)SENSOR_CLIENT_GID,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);

    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability failed: %s\n",
            strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * security_drop_root - Drop from UID 0 to the target non-root UID/GID.
 * POSIX requires setgid() before setuid() to prevent privilege escalation
 * via setresuid/setresgid gaps.
 * Returns 0 on success, -1 on error with errno set.
 */
int security_drop_root(uid_t uid, gid_t gid)
{
    int status;

    /* Drop group privilege first */
    status = setgid(gid);
    if (status == -1) {
        fprintf(stderr,
            "security_drop_root: setgid(%d) failed: %s\n",
            (int)gid, strerror(errno));
        return -1;
    }

    /* Drop user privilege */
    status = setuid(uid);
    if (status == -1) {
        /* Best-effort restore of gid to 0 so the process isn't stuck in a bad state */
        setgid(0);
        fprintf(stderr,
            "security_drop_root: setuid(%d) failed: %s\n",
            (int)uid, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * security_init - Full least-privilege security initialization.
 * Must be called AFTER resmgr_attach() and hardware MMIO/IRQ initialization.
 *
 * Steps:
 *   1. Call security_init_hw_privileges() to lock down the four required
 *      microkernel abilities (IO, Interrupt, Mem_Phys, PUBLIC_CHANNEL)
 *      using the single procmgr_ability() tuple format with PROCMGR_AOP_LOCK.
 *   2. Call security_drop_root(uid, gid) to drop from UID 0 to the
 *      target non-root UID/GID (default 1000:1000 for qnxuser).
 *   3. Log every transition to slog2 with clear errno diagnostics.
 *
 * Returns 0 on success, -1 on error with errno set.
 */
int security_init(uid_t target_uid, gid_t target_gid)
{
    int status = 0;

    if (g_security_init_done) {
        errno = EALREADY;
        return -1;
    }

    /* Step 1: Lock down the four required microkernel abilities */
    status = security_init_hw_privileges();
    if (status == -1) {
        fprintf(stderr,
            "security_init: security_init_hw_privileges() failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* Step 2: Drop from UID 0 to the target non-root UID/GID */
    status = security_drop_root(target_uid, target_gid);
    if (status == -1) {
        fprintf(stderr,
            "security_init: security_drop_root() failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* Step 3: Log the security transition to slog2 */
    {
        char logbuf[128];
        int len;

        /* Log the abilities lockdown */
        len = snprintf(logbuf, sizeof(logbuf),
            "security_init: abilities locked -> "
            "IO|INTERRUPT|MEM_PHYS|PUBLIC_CHANNEL allowed and locked");
        if (slog2f(NULL, 0, SLOG2_INFO, "%s", logbuf) < 0) {
            fprintf(stderr,
                "security_init: slog2f log failed: %s\n",
                strerror(errno));
            /* Not fatal - abilities are already locked */
        }

        /* Log the UID/GID drop */
        len = snprintf(logbuf, sizeof(logbuf),
            "security_init: dropped root -> UID=%d GID=%d",
            (int)getuid(), (int)getgid());
        if (slog2f(NULL, 0, SLOG2_INFO, "%s", logbuf) < 0) {
            fprintf(stderr,
                "security_init: slog2f log failed: %s\n",
                strerror(errno));
            /* Not fatal - privilege drop is already complete */
        }
    }

    g_security_init_done = 1;
    return 0;
}

/*
 * security_get_target_uid - Return the target non-root UID (set at compile time
 * via TARGET_UID default 1000, or overridden at runtime).
 */
uid_t security_get_target_uid(void)
{
    return TARGET_UID;
}

/*
 * security_get_target_gid - Return the target non-root GID (set at compile time
 * via TARGET_GID default 1000, or overridden at runtime).
 */
gid_t security_get_target_gid(void)
{
    return TARGET_GID;
}