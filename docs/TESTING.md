# Project Echo — Testing Guide

## 1. Prerequisites

| Requirement | Notes |
|-------------|-------|
| Raspberry Pi 5 | Or any Linux machine for simulation mode |
| Kernel headers | `sudo apt install linux-headers-$(uname -r)` |
| Build tools | `sudo apt install build-essential make` |
| ncurses | `sudo apt install libncurses-dev` |
| Python 3 | For the non-blocking read stress test |

---

## 2. Build and Load

### Build the module

```bash
cd module/
make
```

Verify: `ls -la echo_robot.ko && modinfo echo_robot.ko`

### Build the app

```bash
cd app/
make
```

### Load (simulation mode)

```bash
sudo scripts/load_module.sh
```

### Load (real hardware)

```bash
sudo scripts/load_module.sh sim_mode=0
```

### Verify loading

```bash
lsmod | grep echo_robot
ls -la /dev/echo_robot
cat /proc/echo_stats
dmesg | tail -20
```

### Unload

```bash
sudo scripts/unload_module.sh
```

---

## 3. Test Strategy

The tests are organised by what they validate:

| Category | Tests | What they prove |
|----------|-------|----------------|
| Lifecycle | 1, 2 | Module loads/unloads cleanly, no memory leaks |
| Blocking I/O | 3, 4 | read() blocks, write(REPLAY) blocks — core assessment feature |
| State machine | 5, 6 | Mode transitions work correctly |
| Hardware | 7 | GPIO interrupts and I2C servo control |
| Interfaces | 8, 9 | /proc, ioctl, all return correct data |
| Stress | 10 | Concurrent access, rapid open/close, no deadlocks |

---

## 4. Manual Test Procedures

### Test 1: Load/Unload Cycle

**Purpose:** Module loads and unloads without errors or leaks.

```bash
sudo scripts/load_module.sh
cat /proc/echo_stats
sudo scripts/unload_module.sh
dmesg | tail -15
```

**Pass criteria:**
- `/dev/echo_robot` appears on load, disappears on unload
- `/proc/echo_stats` shows default values (IDLE, 90/90, all counters 0)
- `dmesg` shows clean init and exit messages, no errors/warnings/oops
- Repeated load/unload (5+ cycles) produces no issues

**Expected dmesg on init:**
```
echo: servo: simulation mode
echo: buffer: subsystem created
echo: state: subsystem created (timeout=5000 ms)
echo: joystick: simulation mode (no GPIO)
echo: chardev: registered (237:0)
echo: proc: /proc/echo_stats created
echo: module loaded (sim_mode=1, timeout=5000 ms)
```

**Expected dmesg on exit:**
```
echo: module unloaded
```

### Test 2: Simulation Mode Operation

**Purpose:** All subsystems work without real hardware.

```bash
sudo scripts/load_module.sh
sudo app/echo_app
```

In the app:
1. Select TEACH from menu
2. Issue MOVE command (servo 0, angle 45)
3. Check `cat /proc/echo_stats` in another terminal
4. Check `dmesg | tail` for `servo[0] = 45 (sim)`

**Pass criteria:**
- Stats show updated angle and buffer count
- `dmesg` shows simulated servo messages
- No I2C or GPIO errors

### Test 3: Blocking Read Verification

**Purpose:** `read()` blocks until new data arrives.

```bash
# Terminal 1 — this will HANG (that's correct behaviour)
sudo dd if=/dev/echo_robot bs=64 count=1 | xxd
```

```bash
# Terminal 2 — trigger a state change
sudo app/echo_app
# Issue a MOVE command from the menu
```

**Pass criteria:**
- Terminal 1 hangs until Terminal 2 triggers a change
- Once triggered, Terminal 1 receives 24 bytes (size of `echo_snapshot`)
- `xxd` output shows the snapshot fields

### Test 4: Blocking Write (REPLAY) Verification

**Purpose:** `write(ECHO_CMD_REPLAY)` blocks until replay completes.

1. Load module, start app
2. Enter TEACH mode, record several moves (MOVE commands)
3. Issue REPLAY command from controller menu
4. Observe: the write call does not return until replay finishes

**Pass criteria:**
- Write call blocks for the duration of replay
- `dmesg` shows "replay started (N moves)" then "replay finished"
- App regains control only after the last move plays

### Test 5: State Machine Transitions

**Purpose:** Verify all mode transitions work correctly.

Test each transition:

| From | Action | Expected To |
|------|--------|-------------|
| IDLE | write(TEACH) | TEACH |
| IDLE | joystick input (hardware mode) | TEACH (auto) |
| TEACH | write(REPLAY) | REPLAY |
| TEACH | inactivity timeout (wait 5s) | REPLAY (auto) |
| REPLAY | replay finishes | IDLE |
| REPLAY | write(STOP) | IDLE |
| Any | ioctl(RESET) | IDLE |

Verify each transition by checking `/proc/echo_stats` mode field.

### Test 6: Auto-Replay (Inactivity Timer)

**Purpose:** Timer auto-triggers replay after timeout.

```bash
sudo scripts/load_module.sh timeout_ms=3000
sudo app/echo_app
```

1. Enter TEACH mode, record a few moves
2. Wait 3 seconds without input
3. Check stats — mode should transition TEACH -> REPLAY -> IDLE

**Pass criteria:**
- `dmesg` shows "inactivity timeout — starting replay"
- Followed by "replay started" and "replay finished"
- Mode returns to IDLE

### Test 7: Joystick Input (Hardware Mode Only)

**Purpose:** GPIO interrupts work and servos move.

```bash
sudo scripts/load_module.sh sim_mode=0
```

1. Press each joystick direction, verify `/proc/echo_stats` angle changes
2. Press button, verify mode transitions
3. Check `dmesg` for IRQ handling

**Pass criteria:**
- UP/DOWN changes tilt by +/- 5 degrees
- LEFT/RIGHT changes pan by +/- 5 degrees
- Button toggles IDLE <-> TEACH
- IRQ Count increments with each debounced press

### Test 8: /proc/echo_stats Verification

**Purpose:** All statistics update correctly.

Run through operations and verify each field:

| Field | When it changes |
|-------|----------------|
| Mode | On TEACH/REPLAY/STOP/IDLE transitions |
| Pan Angle | On left/right input or MOVE command |
| Tilt Angle | On up/down input or MOVE command |
| Buffer Used | Increments during TEACH, preserved during REPLAY |
| Total Moves | Increments with each joystick-triggered move |
| Total Replays | Increments each time replay worker starts |
| IRQ Count | Increments with each debounced GPIO interrupt |
| Open Count | Increments on open(), decrements on close() |
| Sim Mode | Reflects module parameter (does not change) |
| Timeout | Reflects module parameter |

### Test 9: ioctl Commands

**Purpose:** Each ioctl works and validates input.

Test from the app controller menu or a custom test program:

| Command | Test | Expected |
|---------|------|----------|
| GET_STATE | Read state, compare with /proc | Matches |
| SET_SPEED | Set to 2, replay | Moves play at 2x speed |
| SET_SPEED | Set to 0 | Returns -EINVAL |
| RESET | Issue reset | Mode=IDLE, angles=90/90, buffer=0 |
| SET_MODE(1) | Set TEACH | Mode becomes TEACH |
| SET_MODE(0) | Set IDLE | Mode becomes IDLE |
| SET_MODE(2) | Set REPLAY | Replay starts |
| SET_MODE(99) | Invalid | Returns -EINVAL |

---

## 5. Automated Stress Tests

Run the stress test script:

```bash
sudo scripts/test_blocking.sh
```

The script performs six tests:

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Device exists | `/dev/echo_robot` is a character device |
| 2 | /proc interface | `/proc/echo_stats` is readable |
| 3 | Concurrent readers (5 procs) | Multiple blocking reads do not deadlock |
| 4 | Rapid open/close (50 cycles) | Reference counting handles fast cycling |
| 5 | Non-blocking read (O_NONBLOCK) | Returns EAGAIN when no data available |
| 6 | Kernel log check | No error/BUG/oops/panic in dmesg |

**Pass criteria:** All tests pass.  Summary shows `X passed, 0 failed`.

---

## 6. Concurrency Tests

These tests target the locking and synchronisation logic:

### Test: Multiple concurrent readers

Open 5 processes all doing blocking `read()` simultaneously.  Trigger a
single state change.  All 5 should wake and receive valid snapshots.

```bash
for i in $(seq 1 5); do
    timeout 5 dd if=/dev/echo_robot bs=64 count=1 > /dev/null 2>&1 &
done
# Trigger a change (e.g., write TEACH via the app)
wait
```

### Test: Reader + writer simultaneously

One thread does continuous blocking reads, another does rapid writes
(TEACH/MOVE/STOP cycle).  No deadlocks or crashes should occur.

### Test: Replay cancellation

Start replay, immediately send STOP.  The replay worker should exit cleanly
via the `should_stop()` check.  No worker hang or zombie workqueue thread.

---

## 7. Edge Cases to Test

| Scenario | Expected behaviour |
|----------|-------------------|
| read() with buffer too small (< 24 bytes) | Returns partial snapshot (min of count and sizeof) |
| write() with buffer too small (< 16 bytes) | Returns -EINVAL |
| write() with unknown command (e.g., command=99) | Returns -EINVAL |
| REPLAY with empty buffer | Replay starts and finishes immediately |
| REPLAY while already replaying | Returns -EBUSY |
| STOP while in IDLE | Harmless no-op |
| RESET during REPLAY | Cancels replay, centers servos |
| Module unload while device open | rmmod fails with "module in use" |
| Ctrl-C during blocking read | read() returns -ERESTARTSYS, process exits |
| Ctrl-C during blocking write(REPLAY) | write() returns -ERESTARTSYS |
| Buffer full (256 moves) during TEACH | Oldest move silently dropped (kfifo_skip) |

---

## 8. Common Issues and Troubleshooting

### Module fails to load

```
insmod: ERROR: could not insert module echo_robot.ko: Invalid module format
```

**Cause:** Compiled against different kernel headers.
**Fix:** `cd module/ && make clean && make`

### Device node not created

```
Device node not created by udev, creating manually...
Failed to find major number
```

**Cause:** `class_create` or `device_create` failed.
**Fix:** Check `dmesg | tail -20`.  Ensure no other module uses the "echo"
class name.

### Permission denied

```
open /dev/echo_robot: Permission denied
```

**Fix:** Run with `sudo`, or add a udev rule:
```bash
echo 'KERNEL=="echo_robot", MODE="0666"' | sudo tee /etc/udev/rules.d/99-echo.rules
sudo udevadm control --reload-rules
```

### I2C errors (hardware mode)

```
echo: servo: I2C adapter 1 not found
```

**Fix:**
1. Enable I2C: `sudo raspi-config` -> Interface Options -> I2C
2. Verify device: `sudo i2cdetect -y 1` (should show 0x40)
3. Check SDA/SCL wiring

### GPIO request failed

```
echo: joystick: GPIO 17 request failed (-16)
```

**Cause:** Pin already in use.
**Fix:** Check `/sys/kernel/debug/gpio`.  Use different pins:
```bash
sudo insmod echo_robot.ko sim_mode=0 gpio_up=5 gpio_down=6
```

### Module cannot be removed

```
rmmod: ERROR: Module echo_robot is in use
```

**Fix:** Close all processes using the device:
```bash
sudo fuser -k /dev/echo_robot
sudo rmmod echo_robot
```

### Kernel oops or panic

Check `dmesg` for full stack trace.  Common causes:
- NULL pointer dereference (subsystem not created)
- Deadlock (calling `cancel_work_sync` from inside the worker)
- Use-after-free (module unloaded while device open)

---

## 9. Expected dmesg Messages

### Clean lifecycle (simulation mode)

```
[  100.000] echo: servo: simulation mode
[  100.001] echo: buffer: subsystem created
[  100.002] echo: state: subsystem created (timeout=5000 ms)
[  100.003] echo: joystick: simulation mode (no GPIO)
[  100.004] echo: chardev: registered (237:0)
[  100.005] echo: proc: /proc/echo_stats created
[  100.006] echo: module loaded (sim_mode=1, timeout=5000 ms)
[  200.000] echo: chardev: opened (count=1)
[  201.000] echo: servo[0] = 45 (sim)
[  206.000] echo: state: inactivity timeout — starting replay
[  206.001] echo: buffer: replay started (3 moves)
[  206.500] echo: buffer: replay finished
[  207.000] echo: chardev: closed (count=0)
[  300.000] echo: module unloaded
```

### Clean lifecycle (hardware mode)

```
[  100.000] echo: servo: PCA9685 initialised on I2C bus 1
[  100.001] echo: buffer: subsystem created
[  100.002] echo: state: subsystem created (timeout=5000 ms)
[  100.003] echo: joystick: initialised (5 GPIO pins)
[  100.004] echo: chardev: registered (237:0)
[  100.005] echo: proc: /proc/echo_stats created
[  100.006] echo: module loaded (sim_mode=0, timeout=5000 ms)
```
