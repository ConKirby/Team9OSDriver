# Project Echo — Architecture

## 1. System Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         USER SPACE                                      │
│  Runs as a normal process. Uses libc. Has its own virtual memory.       │
│  Communicates with the kernel exclusively through system calls.          │
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
│  Runs in privileged mode. Direct hardware access. Shares one address    │
│  space with the rest of the kernel. A crash here = whole system crash.  │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                  /dev/echo_robot                                  │    │
│  │               (character device node)                             │    │
│  │                                                                   │    │
│  │  echo_chardev.c — file_operations:                                │    │
│  │    .open     → echo_open()       ← increments open_count         │    │
│  │    .release  → echo_release()    ← decrements open_count         │    │
│  │    .read     → echo_read()       ← blocks on wq_read,            │    │
│  │    │                                builds state snapshot,        │    │
│  │    │                                copy_to_user()                │    │
│  │    .write    → echo_write()      ← copy_from_user(),             │    │
│  │    │                                dispatches command,           │    │
│  │    │                                blocks on wq_replay_done      │    │
│  │    .ioctl    → echo_ioctl()      ← copy_from/to_user(),          │    │
│  │                                     fast config queries           │    │
│  ├───────────┬──────────────┬──────────────┬────────────────────────┤    │
│  │           │              │              │                         │    │
│  │  echo_state.c    echo_buffer.c   echo_servo.c   echo_joystick.c │    │
│  │  STATE MACHINE   CIRCULAR BUFFER SERVO DRIVER   GPIO / IRQ      │    │
│  │                                                                   │    │
│  │  IDLE ──→ TEACH  kfifo (256)     I2C bus ──→    request_threaded  │    │
│  │    ↑        │     record()       PCA9685        _irq()            │    │
│  │    │        │     replay_worker  set_angle()    debounce (50ms)   │    │
│  │    │     timeout   (workqueue)   get_angle()    5 GPIO pins       │    │
│  │    │        ↓                                                     │    │
│  │    ╰── REPLAY                                                     │    │
│  │                                                                   │    │
│  ├───────────────────────────────────────────────────────────────────┤    │
│  │  echo_proc.c               │  echo_main.c                        │    │
│  │  /proc/echo_stats          │  module_init() → all *_init()       │    │
│  │  seq_file interface        │  module_exit() → all *_cleanup()    │    │
│  └───────────────────────────────────────────────────────────────────┘    │
│                            │                        │                    │
│                     ┌──────┴───────┐         ┌──────┴──────┐             │
│                     │  I2C BUS     │         │  GPIO PINS  │             │
│                     └──────┬───────┘         └──────┬──────┘             │
└────────────────────────────┼────────────────────────┼────────────────────┘
                             ▼                        ▼
                     ┌───────────────┐         ┌─────────────┐
                     │   PCA9685     │         │  5-way       │
                     │   PWM Board   │         │  Joystick    │
                     │  (2 servos)   │         │  (5 buttons) │
                     └───────────────┘         └─────────────┘
```

---

## 2. User Space vs Kernel Space

User space code **cannot touch hardware directly**. Every interaction with the
servo motors or joystick GPIO pins must go through the kernel via system calls.

### What crosses the boundary

| Direction | System Call | Kernel Function | Transfer Function |
|-----------|-----------|-----------------|-------------------|
| User → Kernel | `write(fd, &cmd, sizeof(cmd))` | `echo_write()` | `copy_from_user()` |
| User → Kernel | `ioctl(fd, ECHO_IOC_SET_SPEED, &val)` | `echo_ioctl()` | `copy_from_user()` |
| Kernel → User | `read(fd, &snap, sizeof(snap))` | `echo_read()` | `copy_to_user()` |
| Kernel → User | `ioctl(fd, ECHO_IOC_GET_STATE, &st)` | `echo_ioctl()` | `copy_to_user()` |

### Why copy_from_user / copy_to_user?

User space and kernel space occupy different virtual address spaces. The kernel
**cannot** simply dereference a pointer that came from user space — it must use
`copy_from_user()` and `copy_to_user()` which:

1. Verify the user pointer is valid and accessible
2. Handle page faults safely (return `-EFAULT` instead of crashing)
3. Copy the data between the two address spaces

### Blocking I/O at the boundary

```
USER PROCESS                        KERNEL
─────────────                       ──────
write(fd, {REPLAY}, 16)
       │
       │  1. CPU traps into kernel mode (syscall instruction)
       │  2. Kernel looks up fd → finds echo_fops
       │  3. Calls echo_write(filp, buf, count, ppos)
       │
       ├───────────────────────►  copy_from_user(&cmd, buf, 16)
       │                          │
       │                          ▼
       │                         switch(cmd.command) {
       │                           case REPLAY:
       │                             queue_work(replay_wq, ...)
       │                             wait_event_interruptible(wq_replay_done)
       │                               │
       │   PROCESS SLEEPS  ◄───────────┘  (removed from CPU run queue)
       │   (not burning CPU,                      │
       │    just waiting)                         │
       │                               replay_worker runs on workqueue
       │                               moves servos via I2C
       │                               ...finishes...
       │                               sets replay_finished = true
       │                               wake_up_interruptible(wq_replay_done)
       │                                          │
       │   PROCESS WAKES UP  ◄────────────────────┘
       │
       ├───────────────────────►  return count;
       │
  write() returns 16
```

The user process is **not** busy-waiting. `wait_event_interruptible()` removes
it from the CPU run queue entirely. The process consumes zero CPU while blocked.

---

## 3. Data Flow — A Complete Teach/Replay Cycle

### Step 1: User starts TEACH mode

```
Controller Thread                    Kernel                        Hardware
───────────────                      ──────                        ────────
write(fd, {TEACH})
    ──syscall──►  echo_write()
                  echo_state_set_mode(TEACH)
                    spin_lock(mode_lock)
                    mode = TEACH
                    mod_timer(inactivity_timer, 5s)
                    spin_unlock(mode_lock)
                  wake_up(wq_read)  ──────►  Visualizer unblocks
                  return count                read() returns snapshot
                                              ncurses renders "TEACH"
```

### Step 2: User presses joystick (hardware interrupt)

```
                                     Kernel                        Hardware
                                     ──────                        ────────
                                                                   GPIO pin
                                                                   falls LOW
                                                                      │
                             IRQ fires (hard IRQ top half)  ◄─────────┘
                               return IRQ_WAKE_THREAD
                                      │
                             Threaded IRQ handler (can sleep):
                               check debounce (50ms jiffies)
                               echo_state_handle_input(dev, LEFT)
                                 │
                                 ├─ pan_angle -= 5
                                 ├─ echo_servo_set_angle(PAN, new_angle)
                                 │     mutex_lock(servo_lock)
                                 │     i2c_smbus_write(PCA9685, ...)  ──► servo moves
                                 │     mutex_unlock(servo_lock)
                                 ├─ echo_buffer_record(dev, move)
                                 │     spin_lock(fifo_lock)
                                 │     kfifo_put(move_fifo, move)
                                 │     spin_unlock(fifo_lock)
                                 ├─ mod_timer(inactivity_timer, 5s)   ← reset timeout
                                 └─ wake_up(wq_read)  ──► Visualizer updates
```

### Step 3: Inactivity timeout fires — auto transition to replay

```
                                     Kernel
                                     ──────
                             timer callback (atomic context):
                               spin_lock(mode_lock)
                               mode = REPLAY
                               spin_unlock(mode_lock)
                               queue_work(replay_wq, replay_work)
                               wake_up(wq_read)
```

### Step 4: Replay worker executes recorded moves

```
                                     Kernel                        Hardware
                                     ──────                        ────────
                             replay_worker() (workqueue thread):
                               copy kfifo → kmalloc'd array
                               for each recorded move:
                                 echo_servo_set_angle(move)  ──────► servo moves
                                 msleep(move.delay_ms / speed)
                                 if mode != REPLAY → break
                               mode = IDLE
                               set replay_finished = true
                               wake_up(wq_replay_done)  ──► Controller unblocks
                               wake_up(wq_read)          ──► Visualizer updates
```

---

## 4. Module Subsystem Map

Each `.c` file is an independent subsystem with a clean `_init()` / `_cleanup()`
API. Subsystems communicate only through function calls declared in headers.

```
                    echo_main.c
                    ═══════════
                    THE ORCHESTRATOR
                    Owns struct echo_device
                    Calls init/cleanup on everything
                        │
           ┌────────────┼────────────────────────────┐
           │            │            │                │
           ▼            ▼            ▼                ▼
    echo_chardev   echo_servo   echo_joystick   echo_proc
    ════════════   ══════════   ═════════════   ═════════
    Boundary to    Boundary to  Boundary to     Boundary to
    user space     I2C hardware GPIO hardware   /proc fs
           │            ▲            │
           │            │            │
           ▼            │            ▼
      echo_state ───────┘      (calls echo_state
      ══════════                on button press)
      Business logic
      Mode transitions
           │
           ▼
      echo_buffer
      ═══════════
      Data storage
      Replay engine
```

### Dependency table

| Module | Depends on | Depended on by | Boundary |
|--------|-----------|----------------|----------|
| `echo_main` | all others | nobody | Module lifecycle |
| `echo_chardev` | state, buffer, servo | main | User ↔ Kernel |
| `echo_servo` | nothing | chardev, state, buffer | Kernel ↔ I2C hardware |
| `echo_joystick` | state | main | Kernel ↔ GPIO hardware |
| `echo_state` | servo, buffer | chardev, joystick | Internal logic |
| `echo_buffer` | servo | state, chardev | Internal data |
| `echo_proc` | nothing (reads dev struct) | main | Kernel ↔ /proc fs |

### Why this modularity matters

- **Swappable hardware**: Replace `echo_servo.c` with a different motor driver
  and nothing else changes.
- **Separation of concerns**: `echo_chardev.c` does not know servos exist. It
  calls `echo_state_set_mode()` and the state machine handles the rest.
- **Testability**: The `sim_mode` flag lets `echo_servo` and `echo_joystick`
  skip all hardware access, so the entire blocking I/O stack works on any Linux
  machine without a Raspberry Pi.

---

## 5. Initialisation Order

`echo_main.c` initialises subsystems in dependency order and tears them down in
reverse. If any step fails, the goto-chain cleans up everything already
initialised.

```
module_init (insmod)                 module_exit (rmmod)
════════════════════                 ═══════════════════
1. kzalloc(echo_device)             7. echo_proc_cleanup()
2. init locks, waitqueues, kfifo    6. echo_joystick_cleanup()
3. echo_chardev_init()              5. echo_buffer_cleanup()
4. echo_servo_init()                4. echo_servo_cleanup()
5. echo_buffer_init()               3. echo_chardev_cleanup()
6. echo_joystick_init()             2. (locks freed implicitly)
7. echo_proc_init()                 1. kfree(echo_device)
```

---

## 6. Concurrency and Locking

### Lock inventory

| Lock | Type | Safe for | Protects |
|------|------|----------|----------|
| `servo_lock` | mutex | Sleeping contexts (I2C, ioctl, read) | `servo_pos[2]`, I2C transfers |
| `open_lock` | mutex | Sleeping contexts (open, close) | `open_count` |
| `mode_lock` | spinlock | IRQ and timer callbacks | `enum echo_mode` |
| `fifo_lock` | spinlock | IRQ context | `kfifo` buffer |
| `stat_total_moves` | atomic_t | Everywhere | Counter |
| `stat_replays` | atomic_t | Everywhere | Counter |
| `stat_irq_count` | atomic_t | Everywhere | Counter |

### When to use which

```
mutex       Use when you need to sleep while holding the lock.
            Example: I2C transfers take time, so servo_lock is a mutex.
            CANNOT be held in interrupt/timer context.

spinlock    Use when you might be in interrupt or timer context.
            Example: mode_lock is a spinlock because the timer callback
            reads the mode, and timer callbacks run in atomic context.
            MUST NOT sleep while holding a spinlock.

atomic_t    Use for simple counters that only need increment/read.
            No explicit lock/unlock — the CPU instruction itself is atomic.
```

### Wait queues

| Wait Queue | Woken by | Condition | Sleeper |
|------------|----------|-----------|---------|
| `wq_read` | state changes, servo moves, replay finish | `new_data_avail == true` | `echo_read()` (visualizer thread) |
| `wq_replay_done` | replay worker completion | `replay_finished == true` | `echo_write(REPLAY)` (controller thread) |

---

## 7. State Machine

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
                  │               (5 seconds, or user
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

---

## 8. Shared Data Structures

### echo_ioctl.h — The contract between user and kernel space

This single header is included by **both** the kernel module and the user-space
app. It defines ioctl command numbers and shared structs. The `#ifdef __KERNEL__`
guard selects the right includes:

```c
#ifdef __KERNEL__
#include <linux/ioctl.h>      // kernel header
#else
#include <sys/ioctl.h>        // libc header
#include <linux/types.h>      // __u32, __u16
#endif
```

### struct echo_cmd — User writes commands to the kernel

```
Sent via:  write(fd, &cmd, sizeof(cmd))
Direction: User → Kernel

┌──────────┬──────────┬──────────┬──────────┐
│ command  │ servo_id │  angle   │  speed   │
│  (u32)   │  (u32)   │  (u32)   │  (u32)   │
└──────────┴──────────┴──────────┴──────────┘
  16 bytes total

command = ECHO_CMD_TEACH (1)   → enter teach mode
command = ECHO_CMD_REPLAY (2)  → start replay (blocks until done)
command = ECHO_CMD_STOP (3)    → stop current mode
command = ECHO_CMD_MOVE (4)    → move servo_id to angle
```

### struct echo_state_snapshot — Kernel sends state to user

```
Sent via:  read(fd, &snap, sizeof(snap))
Direction: Kernel → User

┌──────────┬───────────┬────────────┬──────────────┬─────────────┬───────────────┐
│  mode    │ pan_angle │ tilt_angle │ buffer_count │ total_moves │ total_replays │
│  (u32)   │   (u16)   │   (u16)    │    (u32)     │   (u32)     │    (u32)      │
└──────────┴───────────┴────────────┴──────────────┴─────────────┴───────────────┘
  20 bytes total
```

---

## 9. File Map

```
Team9OSDriver/
├── module/                        KERNEL SPACE
│   ├── echo_main.c/h                Module init/exit, struct echo_device
│   ├── echo_chardev.c/h             Character device file_operations
│   ├── echo_servo.c/h               I2C PCA9685 servo control
│   ├── echo_joystick.c/h            GPIO input, threaded IRQ handlers
│   ├── echo_state.c/h               State machine, inactivity timer
│   ├── echo_buffer.c/h              kfifo circular buffer, replay workqueue
│   ├── echo_proc.c/h                /proc/echo_stats (seq_file)
│   ├── echo_ioctl.h                 Shared ioctl definitions
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
    ├── DESIGN.md                    Design decisions
    └── TESTING.md                   Test procedures
```
