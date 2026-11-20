/*=============================================================================
 *  security_init.c - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#define _GNU_SOURCE
#include "security_init.h"
#include <sys/types.h>
#include <string.h>
#include <errno.h>

/*
 * Static state so we only do the security init once per process lifetime.
 * After security_init() runs, the microkernel abilities are locked and
 * the process has dropped to non-root UID/GID.
 */
static int g_security_init_done = 0;

/*
 * security_init - Full least-privilege security initialization.
 * Must be called AFTER resmgr_attach() and hardware MMIO/IRQ initialization.
 *
 * Steps:
 *   1. Deny ALL microkernel abilities via PROCMGR_DN_ALL.
 *      After this, no process code can acquire any ability, even as root.
 *   2. Allow ONLY the four required abilities and lock them permanently:
 *      PROCMGR_AID_MEM_PHYS  (mmap_device_memory)
 *      PROCMGR_AID_INTERRUPT (InterruptAttachEvent)
 *      PROCMGR_AID_IO        (in32/out32)
 *      PROCMGR_AID_PUBLIC_CHANNEL (ChannelCreate, pulse paths)
 *      using PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK.
 *   3. Drop group privilege first (setgid), then user privilege (setuid),
 *      as POSIX requires setgid() before setuid() to prevent privilege
 *      escalation gaps via setresuid/setresgid.
 *   4. Log every transition to slog2 with clear errno diagnostics.
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

    /* --- Step 1: Deny ALL microkernel abilities --- */
    /* PROCMGR_DN_ALL: deny every ability in the system domain.
     * After this call, no ability can be acquired unless explicitly re-enabled
     * by a process running as UID 0 before the lockdown. */
    status = procmgr_ability(0, PROCMGR_DN_ALL);
    if (status == -1) {
        fprintf(stderr,
            "security_init: procmgr_ability DN_ALL failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* --- Step 2: Allow ONLY the four required abilities and lock them --- */
    /* PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK: allow these specific abilities
     * and permanently lock them so no future process (even as root) can enable
     * other abilities. */
    uint32_t allowed_abilities =
        PROCMGR_AID_IO |
        PROCMGR_AID_INTERRUPT |
        PROCMGR_AID_MEM_PHYS |
        PROCMGR_AID_PUBLIC_CHANNEL;

    status = procmgr_ability(allowed_abilities,
                             PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK);
    if (status == -1) {
        fprintf(stderr,
            "security_init: procmgr_ability ALLOW failed: %s\n",
            strerror(errno));
        /* Cannot recover - we've already denied all above. Return error. */
        return -1;
    }

    /* --- Step 3: Drop root privileges --- */
    /* POSIX requires setgid() before setuid() to prevent privilege escalation
     * via setresuid/setresgid gaps. We drop to the target non-root UID/GID
     * (default 1000:1000), which should be an unprivileged user created
     * for the sensor driver during container/VM init. */

    status = setgid(target_gid);
    if (status == -1) {
        fprintf(stderr,
            "security_init: setgid(%d) failed: %s\n",
            (int)target_gid, strerror(errno));
        /* Cannot easily undo the DN_ALL lockdown from here; return error. */
        return -1;
    }

    status = setuid(target_uid);
    if (status == -1) {
        /* Best-effort: try to restore gid to 0 so the process isn't stuck
         * in a bad state, though the ability lockdown is already in effect. */
        setgid(0);
        fprintf(stderr,
            "security_init: setuid(%d) failed: %s\n",
            (int)target_uid, strerror(errno));
        return -1;
    }

    /* --- Step 4: Log the security transition to slog2 --- */
    {
        char logbuf[128];
        int len;
        /* Log the lockdown step */
        len = snprintf(logbuf, sizeof(logbuf),
            "security_init: abilities locked -> "
            "IO|INTERRUPT|MEM_PHYS|PUBLIC_CHANNEL allowed and locked");
        if (slog2f(NULL, 0, SLOG2_INFO, "%s", logbuf) < 0) {
            fprintf(stderr,
                "security_init: slog2f log failed: %s\n",
                strerror(errno));
            /* Not fatal - abilities are already locked */
        }

        /* Log the UID/GID drop step */
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