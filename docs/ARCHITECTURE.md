# Project Echo — Architecture

## 1. System Overview

Project Echo is a Linux loadable kernel module (LKM) character device driver
for Raspberry Pi 5.  It controls a two-axis pan/tilt servo mount via a PCA9685
PWM controller over I2C.  A five-way joystick connected to GPIO pins provides
physical input.  Movements are recorded into a circular buffer and replayed
automatically — hence the name "Echo."

The system has two halves:

- **Kernel module** (`echo_robot.ko`) — 7 source files compiled into one .ko
- **User-space application** (`echo_app`) — 3 source files, ncurses UI

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         USER SPACE                                      │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                    echo_app (main)                               │    │
│  │                                                                  │    │
│  │   fd = open("/dev/echo_robot", O_RDWR)                          │    │
│  │   sigaction(SIGINT, ...)         ← signal handling               │    │
│  │   pthread_create(visualizer)     ← spawns Thread 1               │    │
│  │   pthread_create(controller)     ← spawns Thread 2               │    │
│  │   pthread_join(...)              ← waits for both                │    │
│  │   close(fd)                                                      │    │
│  ├──────────────────────┬───────────────────────────────────────────┤    │
│  │     Thread 1         │         Thread 2                          │    │
│  │     VISUALIZER       │         CONTROLLER                        │    │
│  │                      │                                           │    │
│  │  ncurses UI          │  stdin menu loop                          │    │
│  │                      │                                           │    │
│  │  read(fd, &snap)     │  write(fd, &cmd)   ← TEACH/REPLAY/       │    │
│  │    ↑ BLOCKS here     │    ↑ BLOCKS here      STOP/MOVE           │    │
│  │    │ until kernel     │    │ until kernel                         │    │
│  │    │ wakes wq_read   │    │ wakes             ioctl(fd, ...)     │    │
│  │                      │    │ wq_replay_done      ↑ SET_SPEED      │    │
│  │  Renders dashboard   │                          │ RESET           │    │
│  │  with snap data      │                          │ GET_STATE       │    │
│  │                      │                          │ SET_MODE        │    │
│  └──────────┬───────────┴──────────┬───────────────┘                    │
│             │ read()               │ write() / ioctl()                  │
│═════════════╪══════════════════════╪════════════════════════════════════│
│         SYSTEM CALL BOUNDARY                                            │
│         copy_to_user() ↑           │ copy_from_user() ↓                 │
│═════════════╪══════════════════════╪════════════════════════════════════│
│             ▼                      ▼                                    │
│                         KERNEL SPACE                                    │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                  /dev/echo_robot                                  │    │
│  │               (character device node)                             │    │
│  │                                                                   │    │
│  │  echo_chardev.c — file_operations:                                │    │
│  │    .open     → echo_open()       ← increments open_count         │    │
│  │    .release  → echo_release()    ← decrements open_count         │    │
│  │    .read     → echo_read()       ← blocks on wq_read,            │    │
│  │                                     builds echo_snapshot,         │    │
│  │                                     copy_to_user()                │    │
│  │    .write    → echo_write()      ← copy_from_user(),             │    │
│  │                                     dispatches command,           │    │
│  │                                     blocks on wq_replay_done      │    │
│  │    .ioctl    → echo_ioctl()      ← copy_from/to_user(),          │    │
│  │                                     fast config queries           │    │
│  └─────┬──────────────┬──────────────┬──────────────┬─────────────┘    │
│        │              │              │              │                    │
│  echo_state.c   echo_buffer.c  echo_servo.c  echo_joystick.c          │
│  STATE MACHINE   RING BUFFER   SERVO DRIVER   GPIO / IRQ              │
│                                                                         │
│  IDLE → TEACH   kfifo (256)    I2C PCA9685    request_threaded_irq()   │
│    ↑      │     record()       set_angle()    debounce (50ms)          │
│    │   timeout  replay_worker  get_angle()    5 GPIO pins              │
│    │      ↓     (workqueue)                                             │
│    └─ REPLAY                                                            │
│                                                                         │
│  echo_proc.c               echo_main.c                                 │
│  /proc/echo_stats          module_init → all *_create()                │
│  seq_file interface         module_exit → all *_destroy()               │
│  └──────────────────────────────────────────────────────────────────┘   │
│                       │                        │                        │
│                ┌──────┴───────┐         ┌──────┴──────┐                 │
│                │  I2C BUS     │         │  GPIO PINS  │                 │
│                └──────┬───────┘         └──────┬──────┘                 │
└───────────────────────┼────────────────────────┼────────────────────────┘
                        ▼                        ▼
                ┌───────────────┐         ┌─────────────┐
                │   PCA9685     │         │  5-way       │
                │   PWM Board   │         │  Joystick    │
                │  (2 servos)   │         │  (5 buttons) │
                └───────────────┘         └─────────────┘
```

---

## 2. Callback-Based Subsystem Architecture

The central design principle is **loose coupling through callback structs**.
Subsystems never call each other directly.  Each subsystem receives an `ops`
struct of function pointers at creation time, plus an opaque `void *data`
pointer.  The coordinator (`echo_main.c`) is the only file that knows how
subsystems connect.

```
                        echo_main.c
                        ═══════════
                        THE COORDINATOR
                        Owns struct echo_device
                        Wires all ops callbacks
                        Calls *_create() / *_destroy()
                              │
          ┌───────────────────┼───────────────────────────────┐
          │                   │                   │            │
          ▼                   ▼                   ▼            ▼
   echo_joystick       echo_state          echo_buffer    echo_servo
   ═════════════       ══════════          ══════════     ══════════
   GPIO / IRQ          Business logic      Data storage   Hardware I2C
   ↓                   ↓                   ↓              (leaf — no deps)
   joy_ops:            state_ops:          buf_ops:
    on_direction ──→    move_servo ──→      move_servo ──→
    on_button ──→       get_servo ──→       should_stop ──→
                        record_move ──→     replay_done ──→
                        clear_buffer ──→    notify ──→
                        start_replay ──→
                        cancel_replay ──→
                        notify ──→

          echo_chardev                      echo_proc
          ═════════════                     ═════════
          Gets echo_device* directly        Gets echo_device* directly
          Calls all subsystem public APIs   Reads all subsystem state
```

### Why callbacks?

- **No circular dependencies.**  The joystick does not `#include` the state
  machine.  The buffer does not `#include` the servo driver.  All routing
  happens through `echo_main.c`.
- **Swappable hardware.**  Replace `echo_servo.c` with a different motor
  driver and nothing else changes — the ops callbacks remain the same.
- **Testability.**  Each subsystem can be unit-tested with mock ops.

### Opaque contexts

Each subsystem owns its internal state in a private `struct *_ctx` that is
forward-declared in the header but only defined in the `.c` file:

```c
/* echo_servo.h — the PUBLIC header */
struct echo_servo_ctx;   /* opaque — callers cannot see inside */
struct echo_servo_ctx *echo_servo_create(bool sim_mode);
void echo_servo_destroy(struct echo_servo_ctx *ctx);
int echo_servo_set_angle(struct echo_servo_ctx *ctx, u8 servo_id, u16 angle);
u16 echo_servo_get_angle(struct echo_servo_ctx *ctx, u8 servo_id);

/* echo_servo.c — the PRIVATE implementation */
struct echo_servo_ctx {
    struct i2c_adapter *adapter;
    struct i2c_client  *client;
    bool sim_mode;
    u16 pos[2];
    struct mutex lock;
};
```

Only `echo_chardev.c` and `echo_proc.c` need the full `struct echo_device`
(from `echo_main.h`) because they are the user-space boundary — they query
all subsystem APIs to build snapshots.

---

## 3. The Coordinator: `struct echo_device`

Defined in `echo_main.h`, this is the single master struct that ties
everything together:

```c
struct echo_device {
    /* Subsystem contexts (opaque — never dereferenced in this struct) */
    struct echo_servo_ctx    *servo;
    struct echo_buffer_ctx   *buffer;
    struct echo_state_ctx    *state;
    struct echo_joystick_ctx *joystick;
    struct echo_chardev_ctx  *chardev;
    struct echo_proc_ctx     *proc;

    /* Blocking I/O coordination (shared across chardev, state, buffer) */
    wait_queue_head_t wq_read;          /* blocks read() */
    wait_queue_head_t wq_replay_done;   /* blocks write(REPLAY) */
    bool new_data_avail;                /* condition for wq_read */
    bool replay_finished;               /* condition for wq_replay_done */

    /* Configuration (set once at init, read-only afterwards) */
    bool sim_mode;
    unsigned long timeout_ms;
};
```

The coordinator is deliberately minimal — it holds subsystem pointers and
shared blocking I/O primitives.  All real logic lives in the subsystems.

---

## 4. Callback Wiring (echo_main.c)

`echo_main.c` defines three sets of static callback functions and fills three
`ops` structs.  Here is the complete wiring:

**Buffer ops** (invoked by the replay worker):

| Callback | Routed to |
|----------|-----------|
| `move_servo` | `echo_servo_set_angle()` |
| `should_stop` | `echo_state_get_mode() != REPLAY` |
| `replay_done` | `echo_state_replay_complete()` + wake `wq_replay_done` |
| `notify` | wake `wq_read` |

**State ops** (invoked by the state machine):

| Callback | Routed to |
|----------|-----------|
| `move_servo` | `echo_servo_set_angle()` |
| `get_servo` | `echo_servo_get_angle()` |
| `record_move` | `echo_buffer_record()` |
| `clear_buffer` | `echo_buffer_clear()` |
| `start_replay` | `echo_buffer_start_replay()` |
| `cancel_replay` | `echo_buffer_cancel_replay()` + wake `wq_replay_done` |
| `notify` | wake `wq_read` |

**Joystick ops** (invoked by threaded IRQ handlers):

| Callback | Routed to |
|----------|-----------|
| `on_direction` | `echo_state_handle_input()` |
| `on_button` | `echo_state_set_mode()` or `echo_state_stop()` |

---

## 5. Initialisation and Teardown

`echo_main.c` creates subsystems in dependency order and tears them down in
reverse.  If any step fails, the goto-chain cleans up everything already
initialised.

```
module_init (insmod)                   module_exit (rmmod)
════════════════════                   ═══════════════════
1. kzalloc(echo_device)               7. echo_proc_destroy()
2. init_waitqueue_head(wq_read,        6. echo_chardev_destroy()
   wq_replay_done)                     5. echo_joystick_destroy()
3. echo_servo_create(sim_mode)         4. echo_state_destroy()
4. echo_buffer_create(&buf_ops)        3. echo_buffer_destroy()
5. echo_state_create(&state_ops)       2. echo_servo_destroy()
6. echo_joystick_create(&joy_ops)      1. kfree(echo_device)
7. echo_chardev_create(echo_dev)
8. echo_proc_create(echo_dev)
```

Dependency reasoning:

- Servo is created first (leaf, no deps).
- Buffer needs buf_ops (which call servo and state), but at creation only
  stores the function pointers — it does not call them until replay starts.
- State needs state_ops (which call servo and buffer).
- Joystick needs joy_ops (which call state).
- Chardev and proc need the fully-wired echo_device pointer.

---

## 6. User Space vs Kernel Space

User space code **cannot touch hardware directly**.  Every servo command or
joystick read must cross the kernel boundary via system calls.

### What crosses the boundary

| Direction | System Call | Kernel Function | Transfer |
|-----------|-----------|-----------------|----------|
| User -> Kernel | `write(fd, &cmd, 16)` | `echo_write()` | `copy_from_user()` |
| User -> Kernel | `ioctl(fd, SET_SPEED, &val)` | `echo_ioctl()` | `copy_from_user()` |
| Kernel -> User | `read(fd, &snap, 24)` | `echo_read()` | `copy_to_user()` |
| Kernel -> User | `ioctl(fd, GET_STATE, &st)` | `echo_ioctl()` | `copy_to_user()` |

### Why copy_from_user / copy_to_user?

User and kernel address spaces are separate.  The kernel cannot simply
dereference a user pointer — `copy_from_user()` / `copy_to_user()`:

1. Verify the user pointer is valid and accessible
2. Handle page faults safely (return `-EFAULT` instead of crashing)
3. Copy data between the two address spaces

---

## 7. Blocking I/O — The Core Assessment Feature

### Blocking read

`echo_read()` blocks the calling process on `wq_read` until `new_data_avail`
becomes true:

```c
if ((filp->f_flags & O_NONBLOCK) && !READ_ONCE(dev->new_data_avail))
    return -EAGAIN;                    /* O_NONBLOCK path */

if (wait_event_interruptible(dev->wq_read,
                             READ_ONCE(dev->new_data_avail)))
    return -ERESTARTSYS;               /* signal interrupted */

WRITE_ONCE(dev->new_data_avail, false);
build_snapshot(dev, &snap);
copy_to_user(buf, &snap, sizeof(snap));
```

Events that wake `wq_read`:
- Joystick input (servo moved)
- Mode transitions (IDLE -> TEACH -> REPLAY -> IDLE)
- Each step of replay playback
- Explicit write commands (TEACH, STOP, MOVE)
- ioctl RESET

### Blocking write (REPLAY)

`echo_write()` with `ECHO_CMD_REPLAY` blocks on `wq_replay_done`:

```c
WRITE_ONCE(dev->replay_finished, false);
ret = echo_state_start_replay(dev->state);

if (wait_event_interruptible(dev->wq_replay_done,
                             READ_ONCE(dev->replay_finished)))
    return -ERESTARTSYS;
```

The replay worker calls `ops->replay_done()` when finished, which sets
`replay_finished = true` and wakes `wq_replay_done`.

### Blocking write sequence diagram

```
USER PROCESS                        KERNEL
─────────────                       ──────
write(fd, {REPLAY}, 16)
       │
       │  1. CPU traps into kernel mode (syscall)
       │  2. Kernel looks up fd → finds echo_fops
       │  3. Calls echo_write()
       │
       ├──────────────────────►  copy_from_user(&cmd, buf, 16)
       │                          switch(cmd.command) {
       │                            case REPLAY:
       │                              echo_state_start_replay()
       │                              wait_event(wq_replay_done, ...)
       │                                │
       │   PROCESS SLEEPS  ◄────────────┘  (removed from CPU run queue)
       │   (zero CPU usage)                        │
       │                               replay_worker runs on workqueue
       │                               moves servos via I2C
       │                               ...finishes...
       │                               sets replay_finished = true
       │                               wake_up(wq_replay_done)
       │                                          │
       │   PROCESS WAKES UP  ◄────────────────────┘
       │
       ├──────────────────────►  return count
       │
  write() returns 16
```

---

## 8. Data Flow — A Complete Teach/Replay Cycle

### Phase 1: Joystick press triggers teach mode

```
Hardware          Kernel                                        User Space
────────          ──────                                        ──────────
GPIO pin
falls LOW
    │
    └───► IRQ fires (hard IRQ top half)
            return IRQ_WAKE_THREAD
                │
          Threaded IRQ handler (can sleep):
            debounce check (50ms jiffies window)
            joy_ops->on_direction(dev, SERVO_PAN, -5)
                │
          echo_main.c routes to:
            echo_state_handle_input(state, PAN, -5)
                │
          echo_state.c:
            auto-enters TEACH (if IDLE)
            new_angle = cur_angle + delta
            state_ops->move_servo(dev, PAN, new_angle)
                │
          echo_main.c routes to:
            echo_servo_set_angle(servo, PAN, 85)  ──► servo moves
                │
          echo_state.c continues:
            state_ops->record_move(dev, &move)
                │
          echo_main.c routes to:
            echo_buffer_record(buffer, &move)
                │
          echo_state.c continues:
            mod_timer(inactivity_timer, jiffies + 5s)
            state_ops->notify(dev)
                │
          echo_main.c:
            WRITE_ONCE(new_data_avail, true)
            wake_up_interruptible(wq_read)
                                                          ──► Visualizer
                                                              unblocks
                                                              read()
                                                              renders UI
```

### Phase 2: Inactivity timeout triggers replay

```
          Kernel
          ──────
          timer callback fires (softirq context):
            spin_lock_irqsave(mode_lock)
            mode = REPLAY
            spin_unlock_irqrestore(mode_lock)
            state_ops->start_replay(dev)
                │
          echo_main.c routes to:
            echo_buffer_start_replay(buffer)
            queue_work(replay_wq, replay_work)
```

### Phase 3: Replay worker plays back moves

```
          Kernel                                          Hardware
          ──────                                          ────────
          replay_worker() on workqueue:
            drain kfifo → kmalloc'd array
            push entries back (preserve recording)
            for each move:
              check should_stop (mode != REPLAY?)
              msleep(delay_ms / replay_speed)
              buf_ops->move_servo(dev, id, angle)  ──► servo moves
              buf_ops->notify(dev)                 ──► wq_read woken
            buf_ops->replay_done(dev)
                │
          echo_main.c:
            echo_state_replay_complete(state)   ← mode = IDLE
            WRITE_ONCE(replay_finished, true)
            wake_up(wq_replay_done)             ← unblocks write(REPLAY)
            wake_up(wq_read)                    ← Visualizer updates
```

---

## 9. State Machine

```
              ┌──────────────────────┐
              │                      │
              ▼                      │
        ┌──────────┐                 │
        │          │   joystick      │
        │   IDLE   │ ──press──►┌───────────┐
        │          │           │           │
        └──────────┘           │   TEACH   │
              ▲                │           │
              │                └─────┬─────┘
              │                      │
              │               inactivity timeout
              │               (5s default, or user
              │                sends REPLAY cmd)
              │                      │
              │                      ▼
              │                ┌───────────┐
              │                │           │
              └── done ────── │  REPLAY   │
                              │           │
                              └───────────┘

Transitions:
  IDLE  → TEACH   : first joystick input or write(TEACH)
  TEACH → REPLAY  : inactivity timer fires or write(REPLAY)
  REPLAY → IDLE   : replay worker finishes or write(STOP)
  Any   → IDLE    : ioctl(RESET)
```

**IDLE:** Servos hold their last position.  No recording.  Joystick input or
TEACH command transitions to TEACH.

**TEACH:** Joystick movements are recorded into kfifo.  The inactivity timer
resets on each input.  When the timer expires, the state machine
auto-transitions to REPLAY.

**REPLAY:** The replay workqueue worker plays back all recorded moves using
their original timing (scaled by `replay_speed`).  A STOP command or button
press cancels replay.  When the sequence ends, mode returns to IDLE.

---

## 10. Concurrency and Locking

### Lock inventory

| Lock | Type | Protects | Context |
|------|------|----------|---------|
| `servo_lock` (in echo_servo_ctx) | mutex | `pos[2]`, I2C transfers | Sleeping (I2C can block) |
| `open_lock` (in echo_chardev_ctx) | mutex | `open_count` | Sleeping (open/close) |
| `mode_lock` (in echo_state_ctx) | spinlock | `enum echo_mode` | Timer callback (softirq) |
| `fifo_lock` (in echo_buffer_ctx) | spinlock | kfifo buffer | Replay worker, record ops |
| `total_moves` (in echo_state_ctx) | atomic_t | Move counter | Everywhere |
| `replay_count` (in echo_buffer_ctx) | atomic_t | Replay counter | Everywhere |
| `irq_count` (in echo_joystick_ctx) | atomic_t | IRQ counter | Everywhere |

### When to use which

- **mutex**: When you need to sleep while holding the lock.  I2C transfers
  can block, so `servo_lock` must be a mutex.  Cannot be held in
  interrupt/timer context.

- **spinlock**: When you might be in interrupt or timer context.  `mode_lock`
  is a spinlock because the inactivity timer callback runs in softirq
  (atomic) context and must read/write the mode.

- **atomic_t**: For simple counters that only need increment/read.  No
  explicit lock/unlock needed — the CPU instruction is inherently atomic.

### Wait queues

| Queue | Woken by | Condition | Sleeper |
|-------|----------|-----------|---------|
| `wq_read` | state changes, servo moves, replay steps | `new_data_avail` | `echo_read()` |
| `wq_replay_done` | replay worker completion | `replay_finished` | `echo_write(REPLAY)` |

### WRITE_ONCE / READ_ONCE

The boolean condition flags (`new_data_avail`, `replay_finished`) use
`WRITE_ONCE` / `READ_ONCE` to prevent compiler optimisations from caching
stale values across the wait_event boundary.

---

## 11. Shared Data Structures

### echo_ioctl.h — The contract between kernel and user space

This header is included by **both** the kernel module and the user-space app.
The `#ifdef __KERNEL__` guard selects the right includes.

### struct echo_cmd (user -> kernel via write)

```
┌──────────┬──────────┬──────────┬──────────┐
│ command  │ servo_id │  angle   │  speed   │
│  (u32)   │  (u32)   │  (u32)   │  (u32)   │
└──────────┴──────────┴──────────┴──────────┘
  16 bytes

command = ECHO_CMD_TEACH (1)   → enter teach mode, clear buffer
command = ECHO_CMD_REPLAY (2)  → start replay (blocks until done)
command = ECHO_CMD_STOP (3)    → stop current mode, return to IDLE
command = ECHO_CMD_MOVE (4)    → move servo_id to angle
```

### struct echo_snapshot (kernel -> user via read and ioctl GET_STATE)

```
┌──────────┬───────────┬────────────┬──────────────┬─────────────┬───────────────┬───────────┐
│  mode    │ pan_angle │ tilt_angle │ buffer_count │ total_moves │ total_replays │ irq_count │
│  (u32)   │   (u16)   │   (u16)    │    (u32)     │   (u32)     │    (u32)      │  (u32)    │
└──────────┴───────────┴────────────┴──────────────┴─────────────┴───────────────┴───────────┘
  24 bytes
```

---

## 12. File Map

```
Team9OSDriver/
├── module/                        KERNEL SPACE
│   ├── echo_main.c/h                Coordinator: init/exit, ops wiring
│   ├── echo_chardev.c/h             Character device file_operations
│   ├── echo_servo.c/h               I2C PCA9685 servo control (leaf)
│   ├── echo_joystick.c/h            GPIO threaded IRQ handlers
│   ├── echo_state.c/h               State machine, inactivity timer
│   ├── echo_buffer.c/h              kfifo ring buffer, replay workqueue
│   ├── echo_proc.c/h                /proc/echo_stats (seq_file)
│   ├── echo_ioctl.h                 Shared structs (kernel + user-space)
│   ├── echo_types.h                 Shared enums, constants, echo_move
│   └── Makefile                     Kbuild: 7 objects → echo_robot.ko
│
├── app/                           USER SPACE
│   ├── echo_app.c                   Main: open, threads, signal, close
│   ├── echo_visualizer.c/h          Thread 1: ncurses + blocking read()
│   ├── echo_controller.c/h          Thread 2: menu + blocking write()/ioctl()
│   └── Makefile                     gcc with -lncurses -lpthread
│
├── scripts/
│   ├── load_module.sh               insmod + device node setup
│   ├── unload_module.sh             rmmod + cleanup
│   └── test_blocking.sh             Stress test suite
│
└── docs/
    ├── ARCHITECTURE.md              This file
    ├── DESIGN.md                    Design decisions and API reference
    ├── TESTING.md                   Test procedures
    └── BUILD_GUIDE.md               Step-by-step build from scratch
```
