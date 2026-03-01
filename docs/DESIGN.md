# Project Echo - Design Document

## 1. Project Overview

Project Echo is a Linux loadable kernel module (LKM) that drives a two-axis
pan/tilt servo mechanism on a Raspberry Pi 5.  A five-way joystick connected
via GPIO lets the user position the servos in real time.  Movements are
recorded into a circular buffer and can be replayed automatically, hence the
name "Echo."

The module exposes a character device (`/dev/echo_robot`), a `/proc` statistics
file, and an `ioctl` interface.  A companion user-space application
(`echo_app`) uses two POSIX threads -- one for ncurses visualisation driven by
blocking `read()`, and one for menu-driven control via `write()` and `ioctl()`.

**Target platform:** Raspberry Pi 5 running a 64-bit Linux kernel (>= 6.x).

## 2. Architecture

```
 +-------------------+          +-------------------+
 | User-Space App    |          |   /proc/echo_stats|
 | (echo_app)        |          |   (read-only)     |
 |                   |          +--------+----------+
 | vis_thread  ctrl  |                   |
 |  (read)   (write/ |                   |
 |            ioctl) |                   |
 +----+---------+----+                   |
      |         |                        |
 -----+---------+--------+--------------+-----  <-- kernel boundary
      |         |        |
 +----+---------+--------+----+
 |       echo_chardev.c       |
 |  open / release / read /   |
 |  write / unlocked_ioctl    |
 +----+---------+--------+----+
      |         |        |
      v         v        v
 +--------+ +--------+ +-----------+
 | echo_  | | echo_  | | echo_     |
 | state  | | buffer | | proc      |
 |  .c    | |  .c    | |  .c       |
 +---+----+ +---+----+ +-----------+
     |          |
     v          v
 +-----------------+     +-------------------+
 | echo_servo.c    |     | echo_joystick.c   |
 | (PCA9685 I2C)   |     | (GPIO threaded    |
 |                 |     |  IRQ handlers)     |
 +---------+-------+     +--------+----------+
           |                      |
 +---------v-------+     +--------v----------+
 | PCA9685 PWM     |     | 5-way Joystick    |
 | Controller      |     | (5 GPIO pins)     |
 | (I2C bus 1,     |     |                   |
 |  addr 0x40)     |     | UP/DOWN/LEFT/     |
 +--------+--------+     | RIGHT/BUTTON      |
          |               +-------------------+
  +-------v-------+
  | Pan Servo     |
  | Tilt Servo    |
  +---------------+
```

### Data Flow

1. **Joystick press** triggers a falling-edge GPIO interrupt.
2. The **threaded IRQ handler** debounces the input and calls
   `echo_state_handle_input()`.
3. The **state machine** auto-enters TEACH mode (if IDLE), computes the new
   servo angle, and records an `echo_move` into the circular buffer.
4. `echo_servo_set_angle()` writes the PWM value to the **PCA9685** over I2C
   (or logs the angle in simulation mode).
5. `new_data_avail` is set and `wq_read` is woken, unblocking any
   **user-space reader**.
6. After `timeout_ms` of inactivity the **inactivity timer** fires and
   transitions to REPLAY mode.
7. The **replay worker** (kernel workqueue) plays back the recorded moves with
   inter-step delays, then returns to IDLE.
8. A blocking `write(ECHO_CMD_REPLAY)` sleeps on `wq_replay_done` until replay
   finishes.

## 3. Module Structure

| File                | Role                                                   |
| ------------------- | ------------------------------------------------------ |
| `echo_main.h`       | Master header: device struct, constants, enums         |
| `echo_main.c`       | Module entry/exit, parameter parsing, init ordering    |
| `echo_ioctl.h`      | ioctl command definitions (shared with user-space)     |
| `echo_chardev.h/c`  | Character device: open/release/read/write/ioctl, cdev  |
| `echo_servo.h/c`    | PCA9685 I2C driver, angle-to-PWM, sim mode fallback    |
| `echo_joystick.h/c` | GPIO request, threaded IRQ, debounce, input dispatch   |
| `echo_state.h/c`    | State machine (IDLE/TEACH/REPLAY), inactivity timer    |
| `echo_buffer.h/c`   | kfifo circular buffer, replay workqueue worker         |
| `echo_proc.h/c`     | /proc/echo_stats via seq_file single_open              |
| `Makefile`          | Kbuild out-of-tree module build                        |

## 4. Key Data Structures

### `struct echo_device`  (echo_main.h)

The single master structure that holds all driver state:

| Field Group       | Members                                  | Purpose                                  |
| ----------------- | ---------------------------------------- | ---------------------------------------- |
| Char device       | `cdev`, `devno`, `dev_class`, `device`   | Character device registration            |
|                   | `open_count`, `open_lock` (mutex)        | Reference counting                       |
| I2C / Servo       | `i2c_adapter`, `i2c_client`, `sim_mode`  | PCA9685 bus handle                       |
|                   | `servo_pos[2]`, `servo_lock` (mutex)     | Current pan/tilt angles                  |
| GPIO / Joystick   | `gpio_pins[5]`, `irqs[5]`               | Pin and IRQ numbers                      |
|                   | `last_irq_jiffies[5]`                   | Per-pin debounce timestamps              |
| State Machine     | `mode`, `mode_lock` (spinlock)           | Current operating mode                   |
|                   | `inactivity_timer`, `timeout_ms`         | Auto-replay trigger                      |
|                   | `replay_speed`                           | Replay speed multiplier                  |
| Command Buffer    | `move_fifo` (kfifo, 256 entries)         | Circular buffer of echo_move             |
|                   | `fifo_lock` (spinlock), `last_move_time` | FIFO protection and timing               |
| Wait Queues       | `wq_read`, `wq_replay_done`             | Blocking I/O                             |
|                   | `new_data_avail`, `replay_finished`      | Condition flags                          |
| Replay Workqueue  | `replay_wq`, `replay_work`              | Async replay execution                   |
| /proc stats       | `stat_total_moves`, `stat_replays`,      | Atomic counters                          |
|                   | `stat_irq_count`                         |                                          |

### `struct echo_move`  (echo_main.h)

A single recorded servo position:

```c
struct echo_move {
    u8  servo_id;    /* ECHO_SERVO_PAN (0) or ECHO_SERVO_TILT (1) */
    u16 angle;       /* 0-180 degrees                             */
    u32 delay_ms;    /* milliseconds since the previous move      */
};
```

### `struct echo_cmd`  (echo_main.h)

A write command from user-space:

```c
struct echo_cmd {
    u32 command;     /* ECHO_CMD_TEACH / REPLAY / STOP / MOVE */
    u32 servo_id;    /* used by MOVE                          */
    u32 angle;       /* used by MOVE                          */
    u32 speed;       /* reserved                              */
};
```

### `struct echo_state_snapshot`  (echo_main.h)

Returned to user-space by `read()`:

```c
struct echo_state_snapshot {
    u32 mode;
    u16 pan_angle;
    u16 tilt_angle;
    u32 buffer_count;
    u32 total_moves;
    u32 total_replays;
};
```

## 5. Blocking I/O Design

### Blocking Read

`read()` blocks the calling process on `wq_read` until `new_data_avail`
becomes true.  Events that set the flag and wake the queue:

- Joystick input (servo moved)
- Mode transition (IDLE -> TEACH -> REPLAY -> IDLE)
- Each step of replay playback
- Explicit write commands (TEACH, STOP, MOVE)
- ioctl RESET

When woken, the driver builds an `echo_state_snapshot` under the appropriate
locks and copies it to user-space.

### Blocking Write (REPLAY)

`write()` with `ECHO_CMD_REPLAY` blocks on `wq_replay_done` until the replay
worker sets `replay_finished = true`.  This gives user-space a synchronous
"play and wait" semantic.

### O_NONBLOCK Support

If the file is opened with `O_NONBLOCK` and no new data is available, `read()`
returns `-EAGAIN` immediately instead of sleeping.

### Interruptibility

Both wait points use `wait_event_interruptible()` and return `-ERESTARTSYS` on
signal delivery, allowing clean Ctrl-C handling.

## 6. State Machine

```
              +------+
              | IDLE |<----------------------------+
              +--+---+                             |
                 |                                 |
    joystick input / write(TEACH)                  |
                 |                                 |
              +--v---+     inactivity timeout      |
              | TEACH|-------------------------+   |
              +--+---+                         |   |
                 |                             |   |
    write(REPLAY) / ioctl(SET_MODE, REPLAY)    |   |
                 |                             |   |
              +--v-----+                       |   |
              | REPLAY |<----------------------+   |
              +--+-----+                           |
                 |                                 |
    replay finishes / write(STOP)                  |
                 |                                 |
                 +---------------------------------+
```

**IDLE:** Servos hold their last position. Joystick input or a TEACH command
transitions to TEACH.

**TEACH:** Joystick movements are recorded into the kfifo buffer. The
inactivity timer resets on each input. If the timer expires the driver
automatically transitions to REPLAY.

**REPLAY:** The replay workqueue worker plays back all recorded moves with
their original timing (scaled by `replay_speed`). A STOP command or button
press cancels replay. When the sequence ends the driver returns to IDLE.

## 7. /proc Interface

**File:** `/proc/echo_stats` (read-only, 0444)

Implemented via `seq_file` / `single_open`.  Output example:

```
=== Project Echo Stats ===
Mode:           IDLE
Pan Angle:      90
Tilt Angle:     90
Buffer Used:    0 / 256
Total Moves:    0
Total Replays:  0
IRQ Count:      0
Open Count:     1
Sim Mode:       yes
Timeout:        5000 ms
```

## 8. ioctl Interface

All ioctl commands use magic number `'E'` (0x45).

| Command              | Direction | Argument                  | Description                                     |
| -------------------- | --------- | ------------------------- | ----------------------------------------------- |
| `ECHO_IOC_SET_SPEED` | `_IOW`    | `__u16` (1 = normal)     | Set replay speed multiplier                     |
| `ECHO_IOC_RESET`     | `_IO`     | none                      | Stop, clear buffer, center servos               |
| `ECHO_IOC_GET_STATE` | `_IOR`    | `struct echo_ioctl_state` | Read current device state                       |
| `ECHO_IOC_SET_MODE`  | `_IOW`    | `__u32` (echo_mode enum) | Force mode transition (0=IDLE, 1=TEACH, 2=REPLAY) |

`ECHO_IOC_GET_STATE` returns:

```c
struct echo_ioctl_state {
    __u32 mode;
    __u16 pan_angle;
    __u16 tilt_angle;
    __u32 buffer_count;
    __u32 total_moves;
    __u32 total_replays;
    __u32 irq_count;
};
```

## 9. Hardware

### PCA9685 PWM Controller

| Parameter     | Value                |
| ------------- | -------------------- |
| I2C Bus       | 1                    |
| I2C Address   | 0x40                 |
| PWM Frequency | 50 Hz (20 ms period) |
| Resolution    | 12-bit (4096 ticks)  |
| Prescale      | 0x79 (121 decimal)   |

### Angle-to-PWM Conversion

```
PWM ticks = 205 + (angle * 205) / 180

  0 degrees  ->  205 ticks  (1.0 ms pulse)
 90 degrees  ->  307 ticks  (1.5 ms pulse)
180 degrees  ->  410 ticks  (2.0 ms pulse)
```

### Servo Channels

| Channel | Servo | Range     | Default |
| ------- | ----- | --------- | ------- |
| 0       | Pan   | 0 - 180 degrees | 90 degrees  |
| 1       | Tilt  | 0 - 180 degrees | 90 degrees  |

## 10. GPIO Pins and Interrupt Handling

### Default Pin Assignments

| Index | Function | Default GPIO | Module Parameter |
| ----- | -------- | ------------ | ---------------- |
| 0     | UP       | 17           | `gpio_up`        |
| 1     | DOWN     | 27           | `gpio_down`      |
| 2     | LEFT     | 22           | `gpio_left`      |
| 3     | RIGHT    | 23           | `gpio_right`     |
| 4     | BUTTON   | 24           | `gpio_button`    |

All pins are configured as inputs with falling-edge triggered interrupts
(`IRQF_TRIGGER_FALLING | IRQF_ONESHOT`).

### Threaded IRQ

Each GPIO uses `request_threaded_irq()`:

- **Hard IRQ handler** (`joystick_hardirq`): returns `IRQ_WAKE_THREAD`
  immediately.  No work is done in hard-interrupt context.
- **Thread function** (`joystick_thread_fn`): runs in process context with
  interrupts enabled.  Identifies which pin fired, applies debouncing, and
  dispatches to the state machine.

### Debouncing

Software debounce with a 50 ms window (`ECHO_DEBOUNCE_MS`).  Each pin tracks
its own `last_irq_jiffies` timestamp; interrupts arriving within the window
are silently discarded.

## 11. Simulation Mode

When the module is loaded with `sim_mode=1` (the default):

- **Servo subsystem:** No I2C adapter is acquired, no PCA9685 registers are
  written.  Angles are stored in `servo_pos[]` and logged via `pr_info`.
- **Joystick subsystem:** No GPIO pins are requested and no IRQs are
  registered.  Input can still be simulated through `write()` commands.
- **All other subsystems** function normally, including the character device,
  state machine, buffer, /proc interface, and ioctl commands.

This allows full development and testing on any Linux machine without a
Raspberry Pi or attached hardware.

## 12. Build and Usage

### Prerequisites

- Raspberry Pi 5 (or any Linux machine for simulation mode)
- Kernel headers matching the running kernel
- `ncurses-dev` (for the user-space application)
- `build-essential`, `make`

### Building

```bash
# Build the kernel module
cd module/
make

# Build the user-space application
cd ../app/
gcc -Wall -Wextra -pthread -lncurses -o echo_app echo_app.c
```

### Loading the Module

```bash
# Simulation mode (default, no hardware required)
cd scripts/
sudo ./load_module.sh

# Real hardware mode
sudo ./load_module.sh sim_mode=0

# Custom GPIO pins
sudo ./load_module.sh sim_mode=0 gpio_up=17 gpio_down=27
```

### Running the Application

```bash
./app/echo_app
```

### Unloading

```bash
cd scripts/
sudo ./unload_module.sh
```

### Checking Status

```bash
cat /proc/echo_stats
dmesg | tail -20
```
