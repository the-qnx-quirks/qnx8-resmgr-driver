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
#ifndef TARGET_UID
#define TARGET_UID  (uid_t)1000
#endif
#ifndef TARGET_GID
#define TARGET_GID  (gid_t)1000
#endif

/*
 * PROCMGR_AOP_* flags for ability lockdown.
 * We allow ONLY the three hardware abilities and deny everything else.
 */
#define HW_ABILITIES_ALLOW \
    (PROCMGR_AID_IO | PROCMGR_AID_INTERRUPT | PROCMGR_AID_MEM_PHYS)

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
 * security_drop_root - Drop from UID 0 to target non-root UID.
 * Must be called AFTER hardware initialization is complete.
 * Returns 0 on success, -1 on error with errno set.
 */
int security_drop_root(void);

/*
 * security_get_target_uid - Return the target non-root UID.
 */
uid_t security_get_target_uid(void);

/*
 * security_get_target_gid - Return the target non-root GID.
 */
gid_t security_get_target_gid(void);

#endif /* !defined(SECURITY_INIT_H_INCLUDED) */