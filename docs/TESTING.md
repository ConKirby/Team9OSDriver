# Project Echo - Testing Guide

## 1. Prerequisites

| Requirement        | Notes                                                    |
| ------------------ | -------------------------------------------------------- |
| Raspberry Pi 5     | Or any Linux machine for simulation mode                 |
| Kernel headers     | `sudo apt install linux-headers-$(uname -r)`             |
| Build tools        | `sudo apt install build-essential make`                  |
| ncurses            | `sudo apt install libncurses-dev`                        |
| Python 3           | Required for the non-blocking read stress test           |

## 2. Build Instructions

### Kernel Module

```bash
cd module/
make
```

A successful build produces `echo_robot.ko`.  Verify:

```bash
ls -la echo_robot.ko
modinfo echo_robot.ko
```

### User-Space Application

```bash
cd app/
gcc -Wall -Wextra -pthread -lncurses -o echo_app echo_app.c
```

## 3. Loading the Module

### Simulation Mode (default)

```bash
cd scripts/
sudo ./load_module.sh
```

This loads with `sim_mode=1`.  No PCA9685 or GPIO hardware is needed.

### Real Hardware Mode

```bash
sudo ./load_module.sh sim_mode=0
```

Requires a PCA9685 at I2C address 0x40 on bus 1 and a five-way joystick wired
to the default GPIO pins (17, 27, 22, 23, 24).

### Custom Parameters

```bash
sudo ./load_module.sh sim_mode=0 gpio_up=5 gpio_down=6 timeout_ms=10000
```

### Verify Loading

```bash
lsmod | grep echo_robot
ls -la /dev/echo_robot
cat /proc/echo_stats
dmesg | tail -20
```

Expected `dmesg` output on clean init:

```
echo_robot: module loaded (sim_mode=1, timeout=5000 ms)
Servo: simulation mode
echo: Buffer subsystem initialized
echo: State machine initialized
echo: Joystick: simulation mode (no GPIO)
echo: chardev registered (237:0)
echo: /proc/echo_stats created
```

## 4. Manual Test Procedures

### Test 1: Load/Unload Cycle

**Purpose:** Confirm the module loads and unloads without errors.

```bash
cd scripts/
sudo ./load_module.sh
cat /proc/echo_stats
sudo ./unload_module.sh
dmesg | tail -15
```

**Expected result:**
- Module loads successfully, `/dev/echo_robot` appears.
- `/proc/echo_stats` shows default values (IDLE mode, 90/90 angles, 0 counts).
- Module unloads cleanly.
- `dmesg` shows no errors, warnings, or kernel oops.

Expected `dmesg` on clean exit:

```
echo_robot: module unloaded
echo: /proc/echo_stats removed
echo: chardev unregistered
echo: Joystick cleaned up
echo: State machine cleaned up
echo: Buffer subsystem cleaned up
```

### Test 2: Simulation Mode Operation

**Purpose:** Verify that all subsystems work without real hardware.

```bash
# In one terminal
sudo ./scripts/load_module.sh
sudo ./app/echo_app
```

In the application:
1. Select TEACH mode from the menu.
2. Issue a MOVE command (e.g., servo 0, angle 45).
3. Verify `/proc/echo_stats` updates:
   ```bash
   cat /proc/echo_stats
   ```
4. Check `dmesg` for `Servo[0] = 45 (sim)` messages.

**Expected result:** Angles update in the stats output.  `dmesg` shows
simulated servo movements.  No I2C or GPIO errors.

### Test 3: Blocking Read Verification

**Purpose:** Confirm that `read()` blocks until new data arrives.

```bash
# Terminal 1 - this will hang (block) waiting for data
sudo dd if=/dev/echo_robot bs=64 count=1 | xxd
```

```bash
# Terminal 2 - trigger a state change via write
# (use the echo_app or a custom C program to write an ECHO_CMD_MOVE)
sudo ./app/echo_app
# Issue a MOVE command
```

**Expected result:**
- Terminal 1 hangs until Terminal 2 triggers a state change.
- Once triggered, Terminal 1 receives an `echo_state_snapshot` struct (24 bytes).
- Verify with `xxd` that the fields match the current state.

### Test 4: Blocking Write Verification

**Purpose:** Confirm that `write(ECHO_CMD_REPLAY)` blocks until replay completes.

1. Load the module and start the application.
2. Enter TEACH mode and record several moves.
3. Issue a REPLAY command.
4. Observe that the write call does not return until the replay worker has
   finished playing back all recorded moves.

**Expected result:** The write call blocks for the duration of the replay. The
application regains control only after the final move is played. Check `dmesg`
for "Replay started" and "Replay finished" messages.

### Test 5: Joystick Input Test (Hardware Mode Only)

**Purpose:** Verify GPIO interrupt handling and servo movement.

```bash
sudo ./scripts/load_module.sh sim_mode=0
```

1. Press each joystick direction and verify servo angles change in
   `/proc/echo_stats`.
2. Press the button and verify mode transitions in the stats output.
3. Check `dmesg` for IRQ handling messages.

**Expected result:**
- UP/DOWN changes tilt angle by +/- 5 degrees.
- LEFT/RIGHT changes pan angle by +/- 5 degrees.
- Button press toggles between IDLE/TEACH modes.
- IRQ Count increments with each press.

### Test 6: Auto-Replay Test

**Purpose:** Verify the inactivity timer triggers automatic replay.

1. Load the module with a short timeout:
   ```bash
   sudo ./scripts/load_module.sh timeout_ms=3000
   ```
2. Enter TEACH mode and record a few moves (via the app or joystick).
3. Wait 3 seconds without any input.
4. Check `/proc/echo_stats` -- mode should transition to REPLAY, then back to
   IDLE when done.

**Expected result:** After 3 seconds of inactivity, `dmesg` shows
"Inactivity timeout -- starting replay", followed by "Replay started" and
"Replay finished".

### Test 7: /proc/echo_stats Verification

**Purpose:** Confirm all statistics update correctly.

```bash
cat /proc/echo_stats
```

Run through a sequence of operations and verify each field updates:

| Field          | When it changes                               |
| -------------- | --------------------------------------------- |
| Mode           | On TEACH/REPLAY/STOP/IDLE transitions         |
| Pan Angle      | On left/right input or MOVE command            |
| Tilt Angle     | On up/down input or MOVE command               |
| Buffer Used    | Increments during TEACH, unchanged during REPLAY |
| Total Moves    | Increments with each servo movement            |
| Total Replays  | Increments each time replay starts             |
| IRQ Count      | Increments with each debounced GPIO interrupt  |
| Open Count     | Increments on open(), decrements on close()    |
| Sim Mode       | Reflects module parameter                      |
| Timeout        | Reflects module parameter                      |

### Test 8: ioctl Commands

**Purpose:** Verify each ioctl command works correctly.

Test each command from user-space (use the echo_app controller menu or a
custom test program):

1. **ECHO_IOC_GET_STATE:** Read state and compare with `/proc/echo_stats`.
2. **ECHO_IOC_SET_SPEED:** Set speed to 2, replay, verify moves play at
   double speed (half the original delay).
3. **ECHO_IOC_RESET:** Reset device, verify mode returns to IDLE, servos
   center at 90/90, buffer clears.
4. **ECHO_IOC_SET_MODE:** Set mode to TEACH (1), verify in stats. Set mode to
   IDLE (0), verify.  Set mode to REPLAY (2), verify replay starts.

**Expected result:** All ioctl calls return 0 on success. Invalid arguments
(e.g., speed = 0, mode = 99) return appropriate error codes (-EINVAL).

## 5. Stress Testing

Run the automated stress test script:

```bash
cd scripts/
sudo ./test_blocking.sh
```

The script performs six tests:

| # | Test                          | What it validates                          |
| - | ----------------------------- | ------------------------------------------ |
| 1 | Device exists                 | `/dev/echo_robot` is a character device    |
| 2 | /proc interface               | `/proc/echo_stats` is readable             |
| 3 | Concurrent readers (5 procs)  | Multiple blocking reads do not deadlock    |
| 4 | Rapid open/close (50 cycles)  | Reference counting handles fast cycling    |
| 5 | Non-blocking read (O_NONBLOCK)| Returns EAGAIN when no data available      |
| 6 | Kernel log check              | No error/BUG/oops/panic in dmesg           |

**Expected result:** All tests pass.  The summary line shows `X passed, 0 failed`.

## 6. Common Issues and Troubleshooting

### Module fails to load

```
insmod: ERROR: could not insert module echo_robot.ko: Invalid module format
```

**Cause:** Module was compiled against different kernel headers.
**Fix:** Rebuild: `cd module/ && make clean && make`

### Device node not created

```
Device node not created by udev, creating manually...
Failed to find major number
```

**Cause:** The `class_create` or `device_create` call failed.
**Fix:** Check `dmesg | tail -20` for the specific error. Ensure no other
module has claimed the "echo" class name.

### Permission denied on /dev/echo_robot

```
open /dev/echo_robot: Permission denied
```

**Fix:** Run the application with `sudo` or add a udev rule:

```bash
echo 'KERNEL=="echo_robot", MODE="0666"' | sudo tee /etc/udev/rules.d/99-echo.rules
sudo udevadm control --reload-rules
```

### I2C errors (hardware mode)

```
Servo: I2C adapter 1 not found
Servo: PCA9685 init failed (-6)
```

**Cause:** I2C bus 1 is not enabled or PCA9685 is not connected.
**Fix:**
1. Enable I2C: `sudo raspi-config` -> Interface Options -> I2C -> Enable
2. Verify the device: `sudo i2cdetect -y 1` (should show 0x40)
3. Check wiring between the PCA9685 and the Pi's SDA/SCL pins.

### GPIO request failed

```
echo: Failed to request GPIO 17 (echo_joy_up): -16
```

**Cause:** GPIO pin is already in use by another driver or overlay.
**Fix:** Check `/sys/kernel/debug/gpio` for pin usage.  Use different pins
via module parameters:

```bash
sudo insmod echo_robot.ko sim_mode=0 gpio_up=5 gpio_down=6
```

### Module cannot be removed (device busy)

```
rmmod: ERROR: Module echo_robot is in use
```

**Cause:** A user-space process still has `/dev/echo_robot` open.
**Fix:** Close the application first, then unload:

```bash
# Find processes using the device
sudo fuser /dev/echo_robot
# Kill if needed
sudo fuser -k /dev/echo_robot
sudo rmmod echo_robot
```

### Kernel oops or panic

Check `dmesg` for a stack trace. Common causes:
- NULL pointer dereference (device struct not initialised)
- Deadlock (lock ordering violation)
- Use-after-free (module unloaded while device still open)

Report the full `dmesg` output and the steps to reproduce.

## 7. Expected dmesg Output

### Clean Module Init (Simulation Mode)

```
[  123.456789] Servo: simulation mode
[  123.456790] echo: Buffer subsystem initialized
[  123.456791] echo: State machine initialized
[  123.456792] echo: Joystick: simulation mode (no GPIO)
[  123.456793] echo: chardev registered (237:0)
[  123.456794] echo: /proc/echo_stats created
[  123.456795] echo_robot: module loaded (sim_mode=1, timeout=5000 ms)
```

### Clean Module Exit

```
[  456.789012] echo_robot: module unloaded
```

Between init and exit, you may see messages such as:

```
[  200.000000] Device opened (count=1)
[  201.000000] Servo[0] = 45 (sim)
[  202.000000] echo: Inactivity timeout - starting replay
[  202.000001] echo: Replay started (3 moves)
[  202.500000] echo: Replay finished
[  203.000000] Device closed (count=0)
```

### Clean Module Init (Hardware Mode)

```
[  123.456789] Servo: PCA9685 initialized on I2C bus 1
[  123.456790] echo: Buffer subsystem initialized
[  123.456791] echo: State machine initialized
[  123.456792] echo: Joystick initialised (5 GPIO pins)
[  123.456793] echo: chardev registered (237:0)
[  123.456794] echo: /proc/echo_stats created
[  123.456795] echo_robot: module loaded (sim_mode=0, timeout=5000 ms)
```
