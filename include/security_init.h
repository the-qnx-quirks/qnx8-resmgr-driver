/*=============================================================================
 *  security_init.h - Least-Privilege Security & Root-Dropping Module
 *  QNX SDP 8.0 - THE QNX QUIRKS - Team: Member 7 (Lidiya)
 *===========================================================================*/
#ifndef SECURITY_INIT_H_INCLUDED
#define SECURITY_INIT_H_INCLUDED

#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <unistd.h>
#include <errno.h>
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
 * PROCMGR_AOP_* flags for ability lockdown in QNX SDP 8.0.
 * We allow ONLY the four required hardware/channel abilities and deny everything else.
 *
 * Step 1: Deny all abilities using PROCMGR_DN_ALL
 * Step 2: Allow only the four specific abilities and lock them down:
 *   PROCMGR_AID_MEM_PHYS   - physically map device memory (mmap_device_memory)
 *   PROCMGR_AID_INTERRUPT  - attach to hardware interrupts (InterruptAttachEvent)
 *   PROCMGR_AID_IO         - perform I/O port reads/writes (in32/out32)
 *   PROCMGR_AID_PUBLIC_CHANNEL - use pulse channels (ChannelCreate / InterruptAttachEvent)
 *
 * After these two procmgr_ability() calls, any attempt to acquire
 * a different ability will fail, even as root. This provides least-privilege
 * enforcement before security_drop_root() drops the process to TARGET_UID/TARGET_GID.
 */
#define HW_ABILITIES_ALLOW \
    (PROCMGR_AID_IO | PROCMGR_AID_INTERRUPT | PROCMGR_AID_MEM_PHYS)

/* Public channel ability added for pulse-driven interrupt paths */
#define HW_ABILITIES_FULL \
    (PROCMGR_AID_IO | PROCMGR_AID_INTERRUPT | PROCMGR_AID_MEM_PHYS | PROCMGR_AID_PUBLIC_CHANNEL)

/*
 * security_init(uid_t target_uid, gid_t target_gid) - Full security initialization.
 * Must be called AFTER resmgr_attach() and hardware MMIO/IRQ initialization is complete.
 * Performs:
 *   1. Denies all microkernel abilities via PROCMGR_DN_ALL
 *   2. Allows ONLY the four required abilities (MEM_PHYS, INTERRUPT, IO, PUBLIC_CHANNEL)
 *      and locks them permanently with PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK
 *   3. Drops from UID 0 to target_uid via setgid()/setuid()
 * Returns 0 on success, -1 on error with errno set.
 */
int security_init(uid_t target_uid, gid_t target_gid);

/*
 * security_get_target_uid - Return the target non-root UID.
 */
uid_t security_get_target_uid(void);

/*
 * security_get_target_gid - Return the target non-root GID.
 */
gid_t security_get_target_gid(void);

#endif /* !defined(SECURITY_INIT_H_INCLUDED) */