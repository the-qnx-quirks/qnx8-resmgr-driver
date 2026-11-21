/*=============================================================================
 *  security_init.c - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#define _GNU_SOURCE
#include "security_init.h"
#include <sys/types.h>
#include <sys/slog2.h>
#include <string.h>
#include <errno.h>

/* Static state so we only do the lockdown once */
static int g_security_init_done = 0;

/*
 * Log one line through the slog2 DEFAULT buffer.
 * slog2 needs a registered buffer; the main program is expected to call
 * slog2_register() and slog2_set_default_buffer() before using this module.
 * If no default buffer exists, fall back to stderr so the message is not lost.
 */
static void security_log(const char *text)
{
    if (slog2f(NULL, 0, SLOG2_INFO, "%s", text) < 0) {
        fprintf(stderr, "security_init: slog2f failed (%s): %s\n",
                strerror(errno), text);
    }
}

int security_init_hw_privileges(void)
{
    int status = 0;

    if (g_security_init_done) {
        errno = EALREADY;
        return -1;
    }

    /* --- Step 1: Lock down ONLY the three hardware abilities --- */
    /* PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK: allow and lock these */
    status = procmgr_ability(HW_ABILITIES_ALLOW,
                             PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability ALLOW failed: "
            "%s\n", strerror(errno));
        return -1;
    }

    /* --- Step 2: Explicitly DENY all remaining abilities --- */
    /* PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK: deny and lock the rest */
    status = procmgr_ability(ALL_ABILITIES_EOL,
                             PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK);
    if (status == -1) {
        fprintf(stderr,
            "security_init_hw_privileges: procmgr_ability DENY failed: "
            "%s\n", strerror(errno));
        return -1;
    }

    /* --- Step 3: Log the transition to slog2 --- */
    /* Since we're still UID 0 at this point, we can write slog2 */
    security_log("security: hw_privileges locked -> "
                 "PROCMGR_AID_IO|AID_INTERRUPT|AID_MEM_PHYS "
                 "allowed and locked");

    g_security_init_done = 1;
    return 0;
}

int security_drop_root(void)
{
    int status = 0;

    if (!g_security_init_done) {
        errno = EPERM;
        fprintf(stderr,
            "security_drop_root: security_init_hw_privileges() must be "
            "called first\n");
        return -1;
    }

    /* --- Step 4: Deny all abilities EOL (extra safety lock) --- */
    /* This ensures even if something later tries to re-enable, it can't */
    status = procmgr_ability(PROCMGR_AID_EOL,
                             PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK);
    if (status == -1) {
        fprintf(stderr,
            "security_drop_root: final procmgr_ability EOL deny failed: "
            "%s\n", strerror(errno));
        /* Not fatal - hardware abilities are already locked */
    }

    /* --- Step 5: Drop group privilege first, then user privilege ---
     *   POSIX requires setgid() before setuid() in most implementations
     *   to prevent privilege escalation via setresuid/setresgid gaps. */
    status = setgid(TARGET_GID);
    if (status == -1) {
        fprintf(stderr,
            "security_drop_root: setgid(%d) failed: %s\n",
            (int)TARGET_GID, strerror(errno));
        return -1;
    }

    status = setuid(TARGET_UID);
    if (status == -1) {
        fprintf(stderr,
            "security_drop_root: setuid(%d) failed: %s\n",
            (int)TARGET_UID, strerror(errno));
        /* Attempt to restore gid - best effort */
        setgid(0);
        return -1;
    }

    /* --- Step 6: Log the UID/GID drop to slog2 --- */
    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf),
            "security: dropped root -> UID=%d GID=%d",
            (int)getuid(), (int)getgid());
        security_log(logbuf);
    }

    g_security_init_done = 0; /* Reset so re-init is possible after exec */
    return 0;
}

uid_t security_get_target_uid(void)
{
    return TARGET_UID;
}

gid_t security_get_target_gid(void)
{
    return TARGET_GID;
}