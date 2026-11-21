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
 * QNX SDP 8.0 Ability Domain Strategy:
 *   - PROCOMGR_ADN_ROOT:   abilities in the root domain
 *   - PROCOMGR_ADN_NONROOT: abilities in the non-root domain
 *   - PROCOMGR_AOP_ALLOW | PROCOMGR_AOP_LOCK: allow and permanently lock
 *     the specified abilities in both domains
 *   - PROCOMGR_AOP_DENY:   deny the specified abilities
 *   - PROCOMGR_AID_EOL:    terminates the tuple list
 *
 * Critical: SetUID/SetGID abilities must be allowed in the ROOT domain only
 * (so security_drop_root() can call setgid()/setuid()), but explicitly
 * denied in the NONROOT domain (so the process cannot re-escalate after dropping).
 *
 * Tuple order matters:
 *   1. Allow MEM_PHYS in both domains (mmap_device_memory)
 *   2. Allow INTERRUPT in both domains (InterruptAttachEvent)
 *   3. Allow IO in both domains (in32/out32)
 *   4. Allow PUBLIC_CHANNEL in both domains (pulse paths)
 *   5. Allow SETUID only in ROOT domain (for security_drop_root())
 *   6. Allow SETGID only in ROOT domain (for security_drop_root())
 *   7. Deny ALL remaining abilities, terminating with PROCOMGR_AID_EOL
 *
 * Returns 0 on success, -1 on error.
 */
int security_init_hw_privileges(void)
{
    int status;

    if (g_security_init_done) {
        errno = EALREADY;
        return -1;
    }

    /* --- Step 1: Allow only the four required abilities in both domains --- */
    /* MEM_PHYS: physically map device memory */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT | PROCOMGR_ADN_NONROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AOP_LOCK | PROCOMGR_AID_MEM_PHYS);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability MEM_PHYS failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* INTERRUPT: attach to hardware interrupts */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT | PROCOMGR_ADN_NONROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AOP_LOCK | PROCOMGR_AID_INTERRUPT);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability INTERRUPT failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* IO: perform I/O port reads/writes */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT | PROCOMGR_ADN_NONROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AOP_LOCK | PROCOMGR_AID_IO);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability IO failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* PUBLIC_CHANNEL: use pulse channels (ChannelCreate, InterruptAttachEvent) */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT | PROCOMGR_ADN_NONROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AOP_LOCK | PROCOMGR_AID_PUBLIC_CHANNEL);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability PUBLIC_CHANNEL failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* --- Step 2: Allow SETUID and SETGID ONLY in the ROOT domain ---
     * This allows security_drop_root() to call setgid()/setuid().
     * Explicitly NOT allowed in NONROOT domain so the process cannot
     * re-escalate after dropping root. */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AID_SETUID);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability SETUID (root) failed: %s\n",
            strerror(errno));
        return -1;
    }

    status = procmgr_ability(
        PROCOMGR_ADN_ROOT,
        PROCOMGR_AOP_ALLOW | PROCOMGR_AID_SETGID);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability SETGID (root) failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* --- Step 3: Deny all remaining abilities in BOTH domains ---
     * The PROCOMGR_AID_EOL terminator is implicit when we use the
     * sub-range approach. We explicitly deny to ensure nothing leaks. */
    status = procmgr_ability(
        PROCOMGR_ADN_NONROOT,
        PROCOMGR_AOP_DENY | PROCOMGR_AID_SETUID | PROCOMGR_AID_SETGID | PROCOMGR_AID_EOL);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability NONROOT DENY failed: %s\n",
            strerror(errno));
        return -1;
    }

    /* Also deny in root domain anything not explicitly allowed above */
    status = procmgr_ability(
        PROCOMGR_ADN_ROOT,
        PROCOMGR_AOP_DENY | PROCOMGR_AID_EOL);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability ROOT DENY failed: %s\n",
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
 *      using per-domain allow/deny tuples with PROCOMGR_AOP_LOCK.
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