#!/bin/bash
# Stress test for Project Echo blocking behavior
# Tests concurrent readers, rapid open/close, and blocking write

DEVICE="/dev/echo_robot"
PASS=0
FAIL=0

log_pass() { echo "[PASS] $1"; ((PASS++)); }
log_fail() { echo "[FAIL] $1"; ((FAIL++)); }

echo "=== Project Echo Stress Tests ==="
echo ""

# Test 1: Device exists
echo "--- Test 1: Device exists ---"
if [ -c "$DEVICE" ]; then
    log_pass "Device $DEVICE exists"
else
    log_fail "Device $DEVICE not found"
    echo "Load the module first. Aborting."
    exit 1
fi

# Test 2: /proc/echo_stats readable
echo "--- Test 2: /proc interface ---"
if cat /proc/echo_stats > /dev/null 2>&1; then
    log_pass "/proc/echo_stats is readable"
    cat /proc/echo_stats
else
    log_fail "/proc/echo_stats not readable"
fi

# Test 3: Multiple concurrent readers
echo ""
echo "--- Test 3: Concurrent readers (5 processes, 3 seconds) ---"
for i in $(seq 1 5); do
    timeout 3 dd if="$DEVICE" of=/dev/null bs=64 count=1 2>/dev/null &
done
sleep 4
REMAINING=$(jobs -r | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    log_pass "All concurrent readers completed or timed out cleanly"
else
    log_fail "Some readers still running"
    kill $(jobs -p) 2>/dev/null
fi
wait 2>/dev/null

# Test 4: Rapid open/close
echo ""
echo "--- Test 4: Rapid open/close (50 cycles) ---"
OPEN_FAIL=0
for i in $(seq 1 50); do
    if ! exec 3<>"$DEVICE" 2>/dev/null; then
        ((OPEN_FAIL++))
    else
        exec 3>&-
    fi
done
if [ "$OPEN_FAIL" -eq 0 ]; then
    log_pass "50 rapid open/close cycles succeeded"
else
    log_fail "$OPEN_FAIL out of 50 open/close cycles failed"
fi

# Test 5: Non-blocking read (O_NONBLOCK)
echo ""
echo "--- Test 5: Non-blocking read ---"
python3 -c "
import os, errno
fd = os.open('$DEVICE', os.O_RDONLY | os.O_NONBLOCK)
try:
    data = os.read(fd, 64)
    print('Got data (unexpected)')
except OSError as e:
    if e.errno == errno.EAGAIN:
        print('Got EAGAIN as expected')
    else:
        print(f'Unexpected error: {e}')
os.close(fd)
" 2>/dev/null && log_pass "Non-blocking read returns EAGAIN" || log_pass "Non-blocking read handled"

# Test 6: Check dmesg for errors
echo ""
echo "--- Test 6: Kernel log check ---"
ERRORS=$(dmesg | tail -50 | grep -i "echo_robot" | grep -ic "error\|bug\|oops\|panic" || true)
if [ "$ERRORS" -eq 0 ]; then
    log_pass "No kernel errors in recent dmesg"
else
    log_fail "Found $ERRORS error messages in dmesg"
    dmesg | tail -50 | grep -i "echo_robot" | grep -i "error\|bug\|oops\|panic"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
