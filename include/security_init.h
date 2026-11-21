/*=============================================================================
 *  security_init.h - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#ifndef SECURITY_INIT_H_INCLUDED
#define SECURITY_INIT_H_INCLUDED

#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <unistd.h>
#include <stdio.h>

/*
 * Target UID/GID for root-dropping: default 1000:1000 (non-root user/group).
 * These correspond to the "sensor_driver" user created during container/VM init.
 */
#ifndef SENSOR_CLIENT_GID
#define SENSOR_CLIENT_GID 1000
#endif
#ifndef SENSOR_DRIVER_UID
#define SENSOR_DRIVER_UID 1000
#endif

#ifndef TARGET_UID
#define TARGET_UID  (uid_t)1000
#endif
#ifndef TARGET_GID
#define TARGET_GID  (gid_t)1000
#endif

/*
 * PROCMGR_AOP_* flags for ability lockdown.
 * We allow ONLY the four required hardware/channel abilities and deny everything else.
 */
#define HW_ABILITIES_ALLOW \
    (PROCMGR_AID_IO | PROCMGR_AID_INTERRUPT | PROCMGR_AID_MEM_PHYS | PROCMGR_AID_PUBLIC_CHANNEL)

#define ALL_ABILITIES_EOL \
    (PROCMGR_AID_EOL)

#define DENY_ALL_AOP \
    (PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK)

/*
 * security_init_hw_privileges - Lock down microkernel abilities WHILE still UID 0.
 * Must be called BEFORE mmap_device_memory() and InterruptAttachEvent().
 * Returns 0 on success, -1 on error with errno set.
 */
int security_init_hw_privileges(void);

/*
 * security_drop_root - Drop from UID 0 to the target non-root UID/GID.
 * POSIX requires setgid() before setuid() to prevent privilege escalation
 * via setresuid/setresgid gaps.
 * uid: target non-root UID (typically TARGET_UID = 1000)
 * gid: target non-root GID (typically TARGET_GID = 1000 or SENSOR_CLIENT_GID)
 * Returns 0 on success, -1 on error with errno set.
 */
int security_drop_root(uid_t uid, gid_t gid);

/*
 * security_init - Full least-privilege security initialization.
 * Must be called AFTER resmgr_attach() and hardware MMIO/IRQ initialization is complete.
 * Performs:
 *   1. security_init_hw_privileges() - lock down the four required microkernel abilities
 *      (IO, INTERRUPT, MEM_PHYS, PUBLIC_CHANNEL) using PROCMGR_DN_ROOT|PROCMGR_DN_NONROOT
 *      and PROCMGR_AOP_ALLOW|PROCMGR_AOP_LOCK.
 *   2. security_drop_root(uid, gid) - drop from UID 0 to the target non-root UID/GID.
 *   3. Log every transition to slog2 with clear errno diagnostics.
 * Returns 0 on success, -1 on error with errno set.
 */
int security_init(uid_t target_uid, gid_t target_gid);

/*
 * security_get_target_gid - Return the target non-root GID.
 */
gid_t security_get_target_gid(void);

#endif /* !defined(SECURITY_INIT_H_INCLUDED) */