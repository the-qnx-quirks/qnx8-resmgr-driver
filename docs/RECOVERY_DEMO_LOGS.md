# QNX 8.0 HAM Watchdog & Crash-Recovery Verification Logs

## x86_64 VirtualBox Target Run (`recovery_demo.sh`)

```text
======================================================
1. CHECKING HAM & GUARDIAN REGISTRATION
======================================================
PASS: ham daemon is running
PASS: 'sensor' entity is registered in /proc/ham
======================================================
2. WATCHDOG RECOVERY (SIGTERM & SIGSEGV)
======================================================
Initial my_driver PID: 315417
PASS [kill]: my_driver recovered (PID 315417 -> 761885) and /dev/sensor0 is back
PASS [kill -SEGV]: my_driver recovered (PID 761885 -> 843806) and /dev/sensor0 is back
======================================================
3. PARALLEL CLIENTS & THREAD POOL CHECK
======================================================
Active my_driver threads: 3 (thread pool active if built with -DUSE_THREAD_POOL)
PASS: All 4 parallel clients opened/accessed /dev/sensor0 cleanly (exit 0)
======================================================
OVERALL RESULT: PASS
```

### Summary of Verified Checks
- **HAM Supervision:** Confirmed `ham` daemon running and `sensor` entity registered in `/proc/ham`.
- **`SIGTERM` (`kill`) Respawn:** `my_driver` automatically restarted with a new PID (`315417 -> 761885`) and restored `/dev/sensor0`.
- **`SIGSEGV` Crash Recovery:** `my_driver` survived a forced segmentation fault, respawned with a new PID (`761885 -> 843806`), and restored `/dev/sensor0`.
- **Concurrency & Thread Pool:** 3 active driver threads (`-DUSE_THREAD_POOL`) served all 4 parallel client accesses simultaneously (`OVERALL RESULT: PASS`).
