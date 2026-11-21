/*=============================================================================
 *  security_init.c - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#define _GNU_SOURCE
#include "security_init.h"
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/slog2.h>

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
 * Returns 0 on success, -1 on error.
 *
 * Step 1: Deny all abilities in the root and non-root domains using
 *         PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT (QNX SDP 8.0 Ability Domain).
 *         The domain flags are terminated explicitly by PROCMGR_AID_EOL
 *         in the ability set passed in Step 2.
 * Step 2: Allow only the four required abilities and lock them:
 *         PROCMGR_AID_IO | PROCMGR_AID_INTERRUPT | PROCMGR_AID_MEM_PHYS | PROCMGR_AID_PUBLIC_CHANNEL
 *         using PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK.
 */
int security_init_hw_privileges(void)
{
    int status = 0;

    if (g_security_init_done) {
        errno = EALREADY;
        return -1;
    }

    /* --- Step 1: Deny all abilities in root and non-root domains ---
     * PROCMGR_DN_ROOT: deny abilities in the root domain
     * PROCMGR_DN_NONROOT: deny abilities in the non-root domain
     * Together they cover all ability domains in QNX SDP 8.0. */
    status = procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability DN_ROOT|DN_NONROOT failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* --- Step 2: Allow only the four required abilities and lock them ---
     * PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK: allow these specific abilities
     * and permanently lock them so no future process (even as root) can enable
     * other abilities. */
    uint32_t allowed =
        PROCMGR_AID_IO |
        PROCMGR_AID_INTERRUPT |
        PROCMGR_AID_MEM_PHYS |
        PROCMGR_AID_PUBLIC_CHANNEL |
        PROCMGR_AID_EOL;

    status = procmgr_ability(allowed,
                             PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability ALLOW failed: %s\n",
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
 *      microkernel abilities (IO, Interrupt, Mem_Phys, Public_Channel).
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