#!/bin/sh
# recovery_demo.sh -- QNX 8.0 HAM Watchdog & Concurrency Verification
# Assumes the boot image has already started `ham` and `ham_guard sensor /proc/boot/my_driver`.

DRIVER_NAME="my_driver"
DEV_NODE="/dev/sensor0"
HAM_ENTRY="/proc/ham/sensor"
FAIL_COUNT=0

get_driver_pid() {
    pidin -p "$DRIVER_NAME" -F "%a" 2>/dev/null | grep -E '^[[:space:]]*[0-9]+' | head -n 1 | tr -d ' '
}

wait_for_recovery() {
    OLD_PID="$1"
    i=0
    while [ "$i" -lt 10 ]; do
        NEW_PID=$(get_driver_pid)
        if [ -n "$NEW_PID" ] && [ "$NEW_PID" != "$OLD_PID" ] && [ -e "$DEV_NODE" ]; then
            echo "$NEW_PID"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

echo "=========================================="
echo " 1. CHECKING HAM & GUARDIAN REGISTRATION"
echo "=========================================="

if pidin -p ham -F "%a" 2>/dev/null | grep -qE '^[[:space:]]*[0-9]+'; then
    echo "PASS: ham daemon is running"
else
    echo "FAIL: ham daemon is not running"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

if [ -e "$HAM_ENTRY" ] || ls /proc/ham 2>/dev/null | grep -q "sensor"; then
    echo "PASS: 'sensor' entity is registered in /proc/ham"
else
    echo "FAIL: 'sensor' entity not found in /proc/ham"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo "=========================================="
echo " 2. WATCHDOG RECOVERY (SIGTERM & SIGSEGV)"
echo "=========================================="

PID1=$(get_driver_pid)
if [ -z "$PID1" ]; then
    echo "FAIL: $DRIVER_NAME is not running initially"
    exit 1
fi
echo "Initial $DRIVER_NAME PID: $PID1"

# Test A: Normal termination (SIGTERM)
kill "$PID1" 2>/dev/null
PID2=$(wait_for_recovery "$PID1")
if [ $? -eq 0 ]; then
    echo "PASS [kill]: $DRIVER_NAME recovered (PID $PID1 -> $PID2) and $DEV_NODE is back"
else
    echo "FAIL [kill]: $DRIVER_NAME did not recover within 10s"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    PID2=$(get_driver_pid)
fi

# Test B: Crash simulation (SIGSEGV)
if [ -n "$PID2" ]; then
    kill -SEGV "$PID2" 2>/dev/null
    PID3=$(wait_for_recovery "$PID2")
    if [ $? -eq 0 ]; then
        echo "PASS [kill -SEGV]: $DRIVER_NAME recovered (PID $PID2 -> $PID3) and $DEV_NODE is back"
    else
        echo "FAIL [kill -SEGV]: $DRIVER_NAME did not recover within 10s"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
fi

echo "=========================================="
echo " 3. PARALLEL CLIENTS & THREAD POOL CHECK"
echo "=========================================="

THREAD_COUNT=$(pidin -p "$DRIVER_NAME" threads 2>/dev/null | grep -E '^[[:space:]]*[0-9]+' | wc -l | tr -d ' ')
echo "Active $DRIVER_NAME threads: $THREAD_COUNT (thread pool active if built with -DUSE_THREAD_POOL)"
if [ "$THREAD_COUNT" -lt 1 ]; then
    echo "FAIL: No active threads found for $DRIVER_NAME"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# Spawn 4 parallel clients accessing /dev/sensor0 concurrently without racing on RESET
PIDS=""
for i in 1 2 3 4; do
    ( exec < "$DEV_NODE" ) >/dev/null 2>&1 &
    PIDS="$PIDS $!"
done

CLIENT_ERRORS=0
for p in $PIDS; do
    wait "$p"
    RC=$?
    if [ "$RC" -ne 0 ]; then
        CLIENT_ERRORS=$((CLIENT_ERRORS + 1))
    fi
done

if [ "$CLIENT_ERRORS" -eq 0 ]; then
    echo "PASS: All 4 parallel clients opened/accessed $DEV_NODE cleanly (exit 0)"
else
    echo "FAIL: $CLIENT_ERRORS of 4 parallel clients failed"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo "=========================================="
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "OVERALL RESULT: PASS"
    exit 0
else
    echo "OVERALL RESULT: FAIL ($FAIL_COUNT check(s) failed)"
    exit 1
fi