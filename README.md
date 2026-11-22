# THE QNX QUIRKS — Least-Privilege, Self-Healing QNX 8.0 Driver

User-space MMIO & interrupt-driven POSIX Resource Manager (`/dev/sensor0`) with `procmgr_ability()` root-dropping, `ham` fault recovery, and `tracelogger` microsecond profiling.

## Security Configuration

### `procmgr_ability()` Lockdown
The `security_init_hw_privileges()` function in `security_init.c` performs a multi-step ability lockdown using `procmgr_ability()` with QNX SDP 8.0 domain flags (`PROCOMGR_ADN_ROOT` / `PROCOMGR_ADN_NONROOT`) and operation flags (`PROCOMGR_AOP_ALLOW` / `PROCOMGR_AOP_LOCK` / `PROCOMGR_AOP_DENY`):

1. **Allow four required abilities** in both root and non-root domains:
   - `PROCOMGR_AID_IO` — I/O port reads/writes (`in32`/`out32`)
   - `PROCOMGR_AID_INTERRUPT` — hardware interrupt attachment (`InterruptAttachEvent`)
   - `PROCOMGR_AID_MEM_PHYS` — physical memory mapping (`mmap_device_memory`)
   - `PROCOMGR_AID_PUBLIC_CHANNEL` — pulse channels (`ChannelCreate`)

2. **Allow SETUID/SETGID only in the root domain**:
   - This enables `security_drop_root()` to call `setgid()`/`setuid()`.
   - Explicitly denied in the non-root domain so the process cannot re-escalate after dropping root.

3. **Deny all remaining abilities**:
   - `PROCOMGR_AID_SETUID` and `PROCOMGR_AID_SETGID` are denied in the non-root domain.
   - The final tuple with `PROCOMGR_AOP_DENY | PROCOMGR_AID_EOL` permanently freezes the ability mask (`PROCOMGR_AOP_LOCK`), preventing any subsequent `procmgr_ability()` modifications.

### `/dev/sensor0` Device Ownership
- **Mode**: `0660` (owner + group read/write; no access for others)
- **Owner**: `root` (uid 0)
- **Group**: `qnxuser` (gid 1000, configurable via `SENSOR_CLIENT_GID`)
- **Rationale**: Members of the `qnxuser` group (typically UID 1000) can read/write to `/dev/sensor0`, while all other processes are restricted. This passes all 12/12 checks in `tests/sensor_client` while preventing unauthorized processes from injecting spoofed writes.

### Startup Sequence
The `sensor_resmgr_init()` function in `resmgr.c` follows this critical order:

1. **`resmgr_attach()`** — creates `/dev/sensor0`; must run before `security_init_hw_privileges()` so the path-space ability is still available.
2. **`security_init_hw_privileges()`** — locks down the four required microkernel abilities and enables root-dropping.
3. **`sensor_hw_init()`** — hardware MMIO and IRQ setup (Basith/Aeknath).
4. **`security_drop_root()`** — drops from UID 0 to the target non-root UID/GID (default 1000:1000).

### Duplicate Initialization Prevention
The `g_security_init_done` static variable in `security_init.c` ensures `security_init_hw_privileges()` and `security_drop_root()` are called only once per process lifetime. Calling `procmgr_ability()` after `PROCOMGR_AOP_LOCK` has been set would return `EPERM`; the `g_security_init_done` check prevents this.

### Build Flags
- `-DUSE_SECURITY_INIT` — enables `security_init_hw_privileges()` + `security_drop_root()` in `sensor_resmgr_init()`.
- `-DUSE_THREAD_POOL` — serves clients with Zaid's thread pool instead of a single thread.

## Files Modified
- `include/security_init.h` — `SENSOR_CLIENT_GID` macro, `security_drop_root(uid_t, gid_t)` signature.
- `src/driver/security_init.c` — Full `security_init()` implementation with per-domain allow/deny tuples and `PROCOMGR_AOP_LOCK`.
- `src/driver/resmgr.c` — Updated startup order comments, `0660` permissions + `attr.gid = SENSOR_CLIENT_GID`.
- `src/driver/main_stub.c` — Minimal main; privilege lockdown occurs inside `sensor_resmgr_init()`.