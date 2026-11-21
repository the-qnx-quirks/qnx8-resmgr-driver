#!/bin/sh
# QNX Quirks end-to-end test for the frozen /dev/sensor0 v1 interface.
# Run ON QNX: sh /tmp/e2e_test.sh
# Optional: CLIENT=/path/to/qnx_sensor_client BASELINE=/path/to/sensor_client sh ...
# NOTE: This test intentionally resets the driver's shared sample queue and
# changes the configured rate. Run on a development target, not in production.

CLIENT=${CLIENT:-/tmp/qnx_sensor_client}
BASELINE=${BASELINE:-/tmp/sensor_client}
DEVICE=${SENSOR_DEVICE:-/dev/sensor0}

passed=0
failed=0

pass() { passed=$((passed + 1)); printf 'PASS  %s\n' "$1"; }
fail() { failed=$((failed + 1)); printf 'FAIL  %s\n' "$1"; }

check_command() {
    label=$1
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

check_contains() {
    label=$1
    output=$2
    expected=$3
    case "$output" in
        *"$expected"*) pass "$label" ;;
        *) fail "$label"; printf '      output: %s\n' "$output" ;;
    esac
}

printf 'QNX QUIRKS /dev/sensor0 end-to-end test\n'
printf 'Client: %s\nBaseline: %s\nDevice: %s\n\n' "$CLIENT" "$BASELINE" "$DEVICE"

if [ ! -e "$DEVICE" ]; then
    printf 'ERROR: %s is missing. Start sensor_resmgr before running tests.\n' "$DEVICE" >&2
    exit 1
fi
if [ ! -x "$CLIENT" ]; then
    printf 'ERROR: %s missing or not executable.\n' "$CLIENT" >&2
    exit 1
fi
if [ ! -x "$BASELINE" ]; then
    printf 'ERROR: %s missing or not executable.\n' "$BASELINE" >&2
    exit 1
fi

# Start from an empty queue so the existing baseline gets deterministic results.
check_command 'initial reset' "$CLIENT" --reset

# Existing team's comprehensive test (12 checks). Treat nonzero as a failure.
printf '\nRunning existing 12-check baseline:\n'
if "$BASELINE"; then pass 'baseline client exits successfully';
else fail 'baseline client exits successfully'; fi

printf '\nRunning telemetry-client integration checks:\n'
check_command 'reset before telemetry checks' "$CLIENT" --reset
check_command 'nonblocking read reports empty' "$CLIENT" --expect-empty

inject_result=$("$CLIENT" --inject 42 2>&1)
if [ $? -eq 0 ]; then
    check_contains 'inject sample 42' "$inject_result" 'injected value=42'
else
    fail 'inject sample 42'
    printf '      output: %s\n' "$inject_result"
fi

latest_result=$("$CLIENT" --latest 2>&1)
if [ $? -eq 0 ]; then
    check_contains 'latest sample reports value 42' "$latest_result" 'value=42'
    check_contains 'latest sample includes timestamp' "$latest_result" 'timestamp_ns='
    check_contains 'latest sample includes sequence' "$latest_result" 'seq='
else
    fail 'latest sample query'
    printf '      output: %s\n' "$latest_result"
fi

read_result=$("$CLIENT" --read 2>&1)
if [ $? -eq 0 ]; then
    check_contains 'queued read returns value 42' "$read_result" 'value=42'
else
    fail 'queued read returns value 42'
    printf '      output: %s\n' "$read_result"
fi
check_command 'queue empty after consuming sample' "$CLIENT" --expect-empty

status_result=$("$CLIENT" --status 2>&1)
if [ $? -eq 0 ]; then
    check_contains 'interface version is 1' "$status_result" 'iface_version=1'
else
    fail 'get status'
    printf '      output: %s\n' "$status_result"
fi

check_command 'set rate to 250 Hz' "$CLIENT" --rate 250
status_result=$("$CLIENT" --status 2>&1)
if [ $? -eq 0 ]; then
    check_contains 'rate is reported as 250 Hz' "$status_result" 'rate_hz=250'
else
    fail 'get status after rate change'
fi

check_command 'final reset' "$CLIENT" --reset
check_command 'queue empty after final reset' "$CLIENT" --expect-empty
check_command 'restore rate to 1000 Hz' "$CLIENT" --rate 1000

printf '\nRESULT: %s passed, %s failed\n' "$passed" "$failed"
if [ "$failed" -ne 0 ]; then
    exit 1
fi
exit 0
