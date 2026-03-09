# Project Echo — Design Document

## 1. Project Overview

Project Echo is a Linux loadable kernel module (LKM) that drives a two-axis
pan/tilt servo mechanism on a Raspberry Pi 5.  A five-way joystick connected
via GPIO lets the user position the servos in real time.  Movements are
recorded into a circular buffer and can be replayed automatically, hence the
name "Echo."

The module exposes a character device (`/dev/echo_robot`), a `/proc` statistics
file, and an `ioctl` interface.  A companion user-space application
(`echo_app`) uses two POSIX threads — one for ncurses visualisation driven by
blocking `read()`, and one for menu-driven control via `write()` and `ioctl()`.

**Target platform:** Raspberry Pi 5 running a 64-bit Linux kernel (>= 6.x).

---

## 2. Module Structure

The module compiles 7 `.c` files into a single `echo_robot.ko` via Kbuild's
multi-object mechanism:

| File | Role |
|------|------|
| `echo_main.c` | Coordinator — module init/exit, parameter parsing, ops callback wiring |
| `echo_main.h` | Defines `struct echo_device` (the coordinator struct) |
| `echo_chardev.c/h` | Character device: open/release/read/write/ioctl, cdev registration |
| `echo_servo.c/h` | I2C PCA9685 driver, angle-to-PWM conversion, sim mode fallback |
| `echo_joystick.c/h` | GPIO request, threaded IRQ, debounce, callback dispatch |
| `echo_state.c/h` | State machine (IDLE/TEACH/REPLAY), inactivity timer |
| `echo_buffer.c/h` | kfifo circular buffer, replay workqueue worker |
| `echo_proc.c/h` | /proc/echo_stats via seq_file single_open |
| `echo_ioctl.h` | Shared ioctl/command/snapshot definitions (kernel + user-space) |
| `echo_types.h` | Shared enums, constants, and `struct echo_move` |

---

## 3. Key Data Structures

### `struct echo_device` (echo_main.h) — The Coordinator

The single master structure.  Holds opaque pointers to all subsystem contexts
plus shared blocking I/O primitives.  Subsystems do NOT include this header —
only `echo_main.c`, `echo_chardev.c`, and `echo_proc.c` see its layout.

| Field Group | Members | Purpose |
|-------------|---------|---------|
| Subsystem contexts | `*servo`, `*buffer`, `*state`, `*joystick`, `*chardev`, `*proc` | Opaque pointers to private subsystem state |
| Blocking I/O | `wq_read`, `wq_replay_done`, `new_data_avail`, `replay_finished` | Wait queues + condition flags |
| Configuration | `sim_mode`, `timeout_ms` | Set once at init, read-only thereafter |

### `struct echo_move` (echo_types.h) — A Recorded Servo Position

```c
struct echo_move {
    u8  servo_id;   /* ECHO_SERVO_PAN (0) or ECHO_SERVO_TILT (1) */
    u16 angle;      /* 0–180 degrees */
    u32 delay_ms;   /* milliseconds since previous move */
};
```

### `struct echo_cmd` (echo_ioctl.h) — Write Command from User-Space

```c
struct echo_cmd {
    __u32 command;   /* ECHO_CMD_TEACH / REPLAY / STOP / MOVE */
    __u32 servo_id;  /* used by MOVE */
    __u32 angle;     /* used by MOVE */
    __u32 speed;     /* reserved */
};
```

### `struct echo_snapshot` (echo_ioctl.h) — State Returned to User-Space

```c
struct echo_snapshot {
    __u32 mode;           /* 0=IDLE, 1=TEACH, 2=REPLAY */
    __u16 pan_angle;      /* 0–180 */
    __u16 tilt_angle;     /* 0–180 */
    __u32 buffer_count;   /* moves in kfifo */
    __u32 total_moves;    /* lifetime counter */
    __u32 total_replays;  /* lifetime counter */
    __u32 irq_count;      /* debounced GPIO interrupts */
};
```

### Subsystem Private Contexts

Each subsystem owns a private context struct visible only within its `.c` file:

| Subsystem | Private Context | Key Fields |
|-----------|----------------|------------|
| echo_servo | `echo_servo_ctx` | `i2c_adapter*`, `i2c_client*`, `pos[2]`, `mutex lock`, `sim_mode` |
| echo_buffer | `echo_buffer_ctx` | `DECLARE_KFIFO(move_fifo, 256)`, `spinlock fifo_lock`, `workqueue_struct*`, `work_struct`, `replay_speed`, `atomic_t replay_count` |
| echo_state | `echo_state_ctx` | `enum echo_mode`, `spinlock mode_lock`, `timer_list inactivity_timer`, `ktime_t last_move_time`, `atomic_t total_moves` |
| echo_joystick | `echo_joystick_ctx` | `gpio_pins[5]`, `irqs[5]`, `last_irq_jiffies[5]`, `atomic_t irq_count` |
| echo_chardev | `echo_chardev_ctx` | `cdev`, `dev_t devno`, `class*`, `device*`, `open_count`, `mutex open_lock` |
| echo_proc | `echo_proc_ctx` | `proc_dir_entry*`, `echo_device*` |

---

## 4. Blocking I/O Design

### Blocking Read

`echo_read()` blocks on `wq_read` until `new_data_avail` becomes true.
Events that set the flag and wake the queue:

- Joystick input (servo moved)
- Mode transition (IDLE -> TEACH -> REPLAY -> IDLE)
- Each step of replay playback
- Explicit write commands (TEACH, STOP, MOVE)
- ioctl RESET

When woken, the driver builds an `echo_snapshot` under the appropriate locks
and copies it to user-space.

### Blocking Write (REPLAY)

`write()` with `ECHO_CMD_REPLAY` blocks on `wq_replay_done` until the replay
worker sets `replay_finished = true`.  This gives user-space a synchronous
"play and wait" semantic.

### O_NONBLOCK Support

If the file is opened with `O_NONBLOCK` and no new data is available,
`read()` returns `-EAGAIN` immediately instead of sleeping.

### Interruptibility

Both wait points use `wait_event_interruptible()` and return `-ERESTARTSYS`
on signal delivery, allowing clean Ctrl-C handling.

---

## 5. State Machine

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

**IDLE:** Servos hold their last position.  Joystick input or a TEACH command
transitions to TEACH.  When entering TEACH via `echo_state_set_mode()`, the
buffer is cleared and the inactivity timer starts.

**TEACH:** Joystick movements are recorded into the kfifo buffer with
timestamps.  The inactivity timer resets on each input.  If the timer expires
the state machine auto-transitions to REPLAY.

**REPLAY:** The replay workqueue worker plays back all recorded moves with
their original timing (divided by `replay_speed`).  A STOP command or button
press cancels replay.  The recording is preserved (snapshot/restore pattern)
so it can be replayed again.

---

## 6. /proc Interface

**File:** `/proc/echo_stats` (read-only, mode 0444)

Implemented via `seq_file` / `single_open`.  Queries all subsystem contexts
through the `echo_device` back-pointer.

Output example:

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

---

## 7. ioctl Interface

All ioctl commands use magic number `'E'` (0x45).

| Command | Direction | Argument | Description |
|---------|-----------|----------|-------------|
| `ECHO_IOC_SET_SPEED` | `_IOW` | `__u16` (1 = normal) | Set replay speed multiplier |
| `ECHO_IOC_RESET` | `_IO` | none | Stop, clear buffer, center servos (90/90) |
| `ECHO_IOC_GET_STATE` | `_IOR` | `struct echo_snapshot` | Read current device state |
| `ECHO_IOC_SET_MODE` | `_IOW` | `__u32` (0/1/2) | Force mode transition |

Error handling:
- `SET_SPEED` with speed=0 returns `-EINVAL`
- `SET_MODE` with value > 2 returns `-EINVAL`
- Unknown ioctl commands return `-ENOTTY`

---

## 8. Hardware

### PCA9685 PWM Controller

| Parameter | Value |
|-----------|-------|
| I2C Bus | 1 |
| I2C Address | 0x40 |
| PWM Frequency | 50 Hz (20 ms period) |
| Resolution | 12-bit (4096 ticks) |
| Prescale | 0x79 (121 decimal) |

### Angle-to-PWM Conversion

```
PWM ticks = 205 + (angle * 205) / 180

  0 degrees  ->  205 ticks  (1.0 ms pulse)
 90 degrees  ->  307 ticks  (1.5 ms pulse)
180 degrees  ->  410 ticks  (2.0 ms pulse)
```

This is integer-only arithmetic — no floating point in kernel space.

### Servo Channels

| Channel | Servo | Range | Default |
|---------|-------|-------|---------|
| 0 | Pan (horizontal) | 0–180 degrees | 90 degrees |
| 1 | Tilt (vertical) | 0–180 degrees | 90 degrees |

### PCA9685 Initialisation Sequence

1. Write MODE1 register = `SLEEP` (0x10) — enter sleep for prescale change
2. Write PRESCALE register = `0x79` — set 50 Hz PWM frequency
3. Write MODE1 = `AUTO_INCREMENT` (0x20) — wake up
4. Wait 500 us — oscillator stabilisation
5. Write MODE1 = `RESTART | AUTO_INCREMENT` (0xA0) — restart PWM outputs
6. Set both channels to center (90 degrees / 307 ticks)

---

## 9. GPIO and Interrupt Handling

### Default Pin Assignments

| Index | Function | Default GPIO | Module Parameter |
|-------|----------|-------------|-----------------|
| 0 | UP | 17 | `gpio_up` |
| 1 | DOWN | 27 | `gpio_down` |
| 2 | LEFT | 22 | `gpio_left` |
| 3 | RIGHT | 23 | `gpio_right` |
| 4 | BUTTON | 24 | `gpio_button` |

All pins are configured as inputs with falling-edge triggered interrupts
(`IRQF_TRIGGER_FALLING | IRQF_ONESHOT`).

### Threaded IRQ

Each GPIO uses `request_threaded_irq()`:

- **Hard IRQ handler** (`joystick_hardirq`): Returns `IRQ_WAKE_THREAD`
  immediately.  No work is done in hard-interrupt context.
- **Thread function** (`joystick_thread_fn`): Runs in process context with
  interrupts enabled.  Identifies which pin fired, applies debouncing, maps
  pin index to servo_id/delta, and fires the appropriate ops callback.

### Debouncing

Software debounce with a 50 ms window (`ECHO_DEBOUNCE_MS`).  Each pin tracks
its own `last_irq_jiffies` timestamp; interrupts arriving within the window
are silently discarded.

### Pin-to-Action Mapping

| Pin Index | ops Callback | Arguments |
|-----------|-------------|-----------|
| UP (0) | `on_direction` | servo_id=TILT, delta=+5 |
| DOWN (1) | `on_direction` | servo_id=TILT, delta=-5 |
| LEFT (2) | `on_direction` | servo_id=PAN, delta=-5 |
| RIGHT (3) | `on_direction` | servo_id=PAN, delta=+5 |
| BUTTON (4) | `on_button` | (no args) |

---

## 10. Buffer and Replay

### kfifo Circular Buffer

- 256 entries of `struct echo_move` (power of 2, required by kfifo)
- Protected by `fifo_lock` spinlock
- When full, the oldest entry is silently dropped (`kfifo_skip`)

### Replay Worker

The replay worker runs on a dedicated single-thread workqueue
(`echo_replay_wq`):

1. **Snapshot**: Drain all kfifo entries into a `kmalloc`'d array
2. **Restore**: Push them back into kfifo (preserves the recording)
3. **Playback**: For each move:
   - Check `should_stop()` — abort if mode changed
   - `msleep(delay_ms / replay_speed)` — minimum 10 ms
   - Call `move_servo()` via ops callback
   - Call `notify()` to wake readers
4. **Complete**: Call `replay_done()` via ops callback

The snapshot/restore pattern means the same recording can be replayed
multiple times without re-recording.

---

## 11. Simulation Mode

When loaded with `sim_mode=1` (the default):

- **Servo subsystem:** No I2C adapter is acquired, no PCA9685 registers are
  written.  Angles are stored in `pos[]` and logged via `pr_info`.
- **Joystick subsystem:** No GPIO pins are requested and no IRQs are
  registered.  Input is driven through `write()` MOVE commands instead.
- **All other subsystems** function normally: character device, state machine,
  buffer, /proc interface, and ioctl commands.

This allows full development and testing on any Linux machine without a
Raspberry Pi or attached hardware.

---

## 12. Module Parameters

| Parameter | Type | Default | Permissions | Description |
|-----------|------|---------|-------------|-------------|
| `sim_mode` | bool | true | 0444 | Simulation mode (no hardware) |
| `timeout_ms` | ulong | 5000 | 0644 | Inactivity timeout in ms |
| `gpio_up` | int | 17 | 0444 | GPIO pin for UP direction |
| `gpio_down` | int | 27 | 0444 | GPIO pin for DOWN direction |
| `gpio_left` | int | 22 | 0444 | GPIO pin for LEFT direction |
| `gpio_right` | int | 23 | 0444 | GPIO pin for RIGHT direction |
| `gpio_button` | int | 24 | 0444 | GPIO pin for BUTTON input |

Parameters with mode 0444 are read-only after loading.  `timeout_ms` has
mode 0644, allowing runtime adjustment via `/sys/module/echo_robot/parameters/timeout_ms`.

---

## 13. User-Space Application

### Architecture

- **Main thread** (`echo_app.c`): Opens `/dev/echo_robot`, installs SIGINT
  handler, spawns two pthreads, waits for both, closes fd.
- **Visualizer thread** (`echo_visualizer.c`): Blocking `read()` loop.
  Each read returns an `echo_snapshot`, rendered with ncurses.
- **Controller thread** (`echo_controller.c`): Menu-driven stdin loop.
  Issues `write()` and `ioctl()` commands based on user input.

### Signal Handling

Global `volatile sig_atomic_t running` flag.  SIGINT handler sets
`running = 0`.  Both threads check the flag and exit gracefully.
The blocked `read()` returns `-ERESTARTSYS` when the signal arrives.

### Build

```bash
cd app/
make       # produces echo_app
```

Requires: `gcc`, `libncurses-dev`, `libpthread`
