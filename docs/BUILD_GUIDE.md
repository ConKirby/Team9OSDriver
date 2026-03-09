# Project Echo — Build From Scratch Guide

This document explains how to recreate the entire Project Echo module from an
empty directory.  It covers the reasoning behind every design decision so the
module can be rebuilt without referencing the existing code.

---

## Step 0: Understand the Goal

Build a Linux kernel module that:

1. Exposes a character device (`/dev/echo_robot`)
2. Uses **blocking I/O** (the core assessment feature):
   - `read()` blocks until device state changes
   - `write(REPLAY)` blocks until replay finishes
3. Controls 2 servos via I2C (PCA9685 PWM controller)
4. Reads a 5-way joystick via GPIO interrupts
5. Records movements and replays them automatically
6. Exposes `/proc/echo_stats` for statistics
7. Has a simulation mode for development without hardware

---

## Step 1: Plan the File Structure

```
module/
├── echo_main.c/h       ← Coordinator (init, exit, ops wiring)
├── echo_chardev.c/h     ← Character device (file_operations)
├── echo_servo.c/h       ← I2C servo driver (leaf, no deps)
├── echo_joystick.c/h    ← GPIO interrupt handler (leaf-ish)
├── echo_state.c/h       ← State machine + timer
├── echo_buffer.c/h      ← Ring buffer + replay worker
├── echo_proc.c/h        ← /proc statistics
├── echo_ioctl.h         ← Shared header (kernel + user-space)
├── echo_types.h         ← Shared constants and types
└── Makefile

app/
├── echo_app.c           ← Main (open, threads, signal handling)
├── echo_visualizer.c/h  ← ncurses UI thread
├── echo_controller.c/h  ← Menu control thread
└── Makefile

scripts/
├── load_module.sh
├── unload_module.sh
└── test_blocking.sh
```

### Why 7 files?

Each file is one subsystem with a clean create/destroy API.  Benefits:

- **No circular dependencies** — subsystems communicate via callback structs
- **Swappable hardware** — replace echo_servo.c without touching anything else
- **Testable** — each subsystem can be tested with mock callbacks

---

## Step 2: Define Shared Types (echo_types.h)

This header is included by every `.c` file.  It defines constants, enums,
and the `echo_move` struct.  It does NOT include `echo_main.h`.

```c
/* Constants */
#define ECHO_DEVICE_NAME    "echo_robot"
#define ECHO_CLASS_NAME     "echo"
#define ECHO_FIFO_SIZE      256        /* must be power of 2 for kfifo */
#define ECHO_NUM_SERVOS     2
#define ECHO_SERVO_PAN      0
#define ECHO_SERVO_TILT     1
#define ECHO_SERVO_CENTER   90
#define ECHO_SERVO_MIN      0
#define ECHO_SERVO_MAX      180
#define ECHO_NUM_GPIO       5
#define ECHO_DEFAULT_TIMEOUT_MS  5000
#define ECHO_DEBOUNCE_MS    50
#define ECHO_ANGLE_STEP     5

/* Operating modes */
enum echo_mode { ECHO_MODE_IDLE=0, ECHO_MODE_TEACH=1, ECHO_MODE_REPLAY=2 };

/* A single recorded movement */
struct echo_move {
    u8  servo_id;
    u16 angle;
    u32 delay_ms;   /* time since previous move */
};
```

---

## Step 3: Define the User-Kernel Contract (echo_ioctl.h)

This header is shared between the kernel module and user-space app.  Use
`#ifdef __KERNEL__` to select the right includes.

```c
#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#endif

/* Write commands */
#define ECHO_CMD_TEACH   1
#define ECHO_CMD_REPLAY  2
#define ECHO_CMD_STOP    3
#define ECHO_CMD_MOVE    4

struct echo_cmd {
    __u32 command, servo_id, angle, speed;  /* 16 bytes */
};

struct echo_snapshot {
    __u32 mode;
    __u16 pan_angle, tilt_angle;
    __u32 buffer_count, total_moves, total_replays, irq_count;  /* 24 bytes */
};

/* ioctl numbers (magic 'E') */
#define ECHO_IOC_MAGIC      'E'
#define ECHO_IOC_SET_SPEED  _IOW(ECHO_IOC_MAGIC, 1, __u16)
#define ECHO_IOC_RESET      _IO(ECHO_IOC_MAGIC, 2)
#define ECHO_IOC_GET_STATE  _IOR(ECHO_IOC_MAGIC, 3, struct echo_snapshot)
#define ECHO_IOC_SET_MODE   _IOW(ECHO_IOC_MAGIC, 4, __u32)
```

---

## Step 4: Build the Servo Subsystem (echo_servo.c/h)

This is a **leaf** subsystem — it depends on nothing else.  Build it first.

### Header (public API)

```c
struct echo_servo_ctx;   /* opaque */
struct echo_servo_ctx *echo_servo_create(bool sim_mode);
void echo_servo_destroy(struct echo_servo_ctx *ctx);
int echo_servo_set_angle(struct echo_servo_ctx *ctx, u8 servo_id, u16 angle);
u16 echo_servo_get_angle(struct echo_servo_ctx *ctx, u8 servo_id);
```

### Implementation details

1. **Private context**: `i2c_adapter*`, `i2c_client*`, `pos[2]`, `mutex lock`
2. **I2C setup**: `i2c_get_adapter(1)` + `i2c_new_client_device()` with
   address 0x40
3. **PCA9685 init**: sleep -> prescale -> wake -> restart (see Design doc)
4. **Angle to PWM**: `ticks = 205 + (angle * 205) / 180` (integer only)
5. **set_angle**: Lock mutex, update pos[], write I2C (or log in sim mode)
6. **get_angle**: Lock mutex, return pos[]
7. **Destroy**: Turn off PWM, unregister I2C client, put adapter, kfree

### Why mutex (not spinlock)?

I2C transfers can block (they sleep while waiting for the bus).  You cannot
sleep while holding a spinlock.

---

## Step 5: Build the Buffer Subsystem (echo_buffer.c/h)

### Design the callback interface

The buffer needs to call other subsystems during replay, but it must not
`#include` them.  Define an ops struct:

```c
struct echo_buffer_ops {
    int  (*move_servo)(void *data, u8 servo_id, u16 angle);
    bool (*should_stop)(void *data);
    void (*replay_done)(void *data);
    void (*notify)(void *data);
};
```

### Implementation details

1. **kfifo**: `DECLARE_KFIFO(move_fifo, struct echo_move, 256)` —
   256 must be power of 2
2. **spinlock**: Protects kfifo (record can happen from threaded IRQ context)
3. **Workqueue**: `create_singlethread_workqueue("echo_replay_wq")` for
   the replay worker
4. **record()**: Lock spinlock, skip oldest if full, put new entry, unlock
5. **Replay worker**:
   - Drain kfifo into kmalloc'd array (under spinlock)
   - Push entries back into kfifo (preserves recording)
   - Loop: check should_stop, msleep(delay/speed), move_servo, notify
   - Call replay_done when finished
6. **cancel_replay()**: `cancel_work_sync()` — waits for worker to finish
7. **Speed**: `WRITE_ONCE(replay_speed, speed)` — atomic single-word write

### Why snapshot/restore?

Draining the kfifo gives us an array we can iterate without holding the
spinlock for the entire replay (which could be seconds).  Pushing entries back
means the recording survives for repeated replays.

---

## Step 6: Build the State Machine (echo_state.c/h)

### Design the callback interface

```c
struct echo_state_ops {
    int  (*move_servo)(void *data, u8 servo_id, u16 angle);
    u16  (*get_servo)(void *data, u8 servo_id);
    void (*record_move)(void *data, const struct echo_move *move);
    void (*clear_buffer)(void *data);
    void (*start_replay)(void *data);
    void (*cancel_replay)(void *data);
    void (*notify)(void *data);
};
```

### Implementation details

1. **Private context**: `enum echo_mode`, `spinlock mode_lock`,
   `timer_list inactivity_timer`, `ktime_t last_move_time`, `atomic_t total_moves`
2. **handle_input()**: Auto-enter TEACH from IDLE, compute new angle,
   move servo, record move with timestamp delta, reset timer
3. **Inactivity timer callback**: Runs in softirq (atomic context).
   Check mode==TEACH, set mode=REPLAY, call start_replay + notify.
4. **start_replay()**: Set mode=REPLAY under spinlock, del_timer_sync,
   call start_replay ops
5. **stop()**: Set mode=IDLE under spinlock, del_timer_sync,
   call cancel_replay ops
6. **replay_complete()**: Called by buffer worker when done.  Just sets
   mode=IDLE.  Does NOT call cancel_replay (would deadlock — we are inside
   the worker that cancel_work_sync would wait for).

### Why spinlock (not mutex) for mode?

The inactivity timer callback runs in softirq context (atomic).  Mutexes
cannot be held in atomic context.  Always use `spin_lock_irqsave` to be
safe even if called from hard IRQ context.

### Timer setup

```c
timer_setup(&ctx->inactivity_timer, inactivity_timer_fn, 0);
mod_timer(&ctx->inactivity_timer, jiffies + msecs_to_jiffies(timeout_ms));
```

---

## Step 7: Build the Joystick Subsystem (echo_joystick.c/h)

### Design the callback interface

```c
struct echo_joystick_ops {
    void (*on_direction)(void *data, u8 servo_id, int delta);
    void (*on_button)(void *data);
};
```

### Implementation details

1. **Private context**: `gpio_pins[5]`, `irqs[5]`, `last_irq_jiffies[5]`,
   `atomic_t irq_count`
2. **GPIO setup loop**: For each pin: `gpio_request()` + `gpio_direction_input()`
   + `gpio_to_irq()` + `request_threaded_irq()`
3. **Hard IRQ**: Just returns `IRQ_WAKE_THREAD` (no work in hard context)
4. **Thread function**: Identify pin by matching irq number, debounce check,
   map pin to servo_id+delta, call ops callback
5. **IRQ flags**: `IRQF_TRIGGER_FALLING | IRQF_ONESHOT`
6. **Cleanup**: Free IRQs and GPIOs in reverse order

### Pin mapping

```
UP    → on_direction(TILT, +5)
DOWN  → on_direction(TILT, -5)
LEFT  → on_direction(PAN, -5)
RIGHT → on_direction(PAN, +5)
BUTTON → on_button()
```

### Why threaded IRQ?

The ops callbacks will eventually call into the state machine, which takes
a spinlock.  Threaded IRQ handlers run in process context with interrupts
enabled, so sleeping operations (like I2C) are safe further down the chain.

---

## Step 8: Build the Character Device (echo_chardev.c/h)

This is the user-space boundary.  Unlike other subsystems, it receives a
full `echo_device*` pointer and calls all subsystem APIs directly.

### Implementation details

1. **Private context**: `cdev`, `dev_t`, `class*`, `device*`, `open_count`,
   `mutex open_lock`, `echo_device *dev`
2. **Registration sequence**:
   - `alloc_chrdev_region()` — dynamic major number
   - `cdev_init()` + `cdev_add()`
   - `class_create()` — creates `/sys/class/echo`
   - `device_create()` — creates `/dev/echo_robot`
3. **open/release**: Increment/decrement `open_count` under mutex
4. **read()**: Check O_NONBLOCK, wait_event_interruptible on wq_read,
   build snapshot, copy_to_user
5. **write()**: copy_from_user into echo_cmd, switch on command:
   - TEACH: `echo_state_set_mode(TEACH)`
   - REPLAY: Set replay_finished=false, start replay, block on wq_replay_done
   - STOP: `echo_state_stop()`
   - MOVE: `echo_servo_set_angle()` + notify readers
6. **ioctl()**: SET_SPEED, RESET, GET_STATE, SET_MODE
7. **Cleanup**: Reverse of registration (device_destroy, class_destroy,
   cdev_del, unregister_chrdev_region)

### build_snapshot helper

```c
static void build_snapshot(struct echo_device *dev, struct echo_snapshot *snap)
{
    snap->mode          = echo_state_get_mode(dev->state);
    snap->pan_angle     = echo_servo_get_angle(dev->servo, PAN);
    snap->tilt_angle    = echo_servo_get_angle(dev->servo, TILT);
    snap->buffer_count  = echo_buffer_count(dev->buffer);
    snap->total_moves   = echo_state_get_total_moves(dev->state);
    snap->total_replays = echo_buffer_get_replay_count(dev->buffer);
    snap->irq_count     = echo_joystick_get_irq_count(dev->joystick);
}
```

---

## Step 9: Build the Proc Interface (echo_proc.c/h)

### Implementation details

1. **Private context**: `proc_dir_entry*`, `echo_device*`
2. Use `proc_create_data()` with `proc_ops` struct
3. Open callback: `single_open(filp, show_fn, pde_data(inode))`
4. Show callback: Query all subsystem APIs via echo_device, print with
   `seq_printf`
5. Cleanup: `remove_proc_entry("echo_stats", NULL)`

---

## Step 10: Wire Everything in echo_main.c

This is the most important file.  It is the ONLY file that knows how
subsystems connect.

### 1. Define module parameters

```c
static bool sim_mode = true;
module_param(sim_mode, bool, 0444);
static unsigned long timeout_ms = 5000;
module_param(timeout_ms, ulong, 0644);
static int gpio_up = 17;  /* etc for all 5 pins */
module_param(gpio_up, int, 0444);
```

### 2. Define notification helpers

```c
static void notify_readers(struct echo_device *dev) {
    WRITE_ONCE(dev->new_data_avail, true);
    wake_up_interruptible(&dev->wq_read);
}

static void signal_replay_done(struct echo_device *dev) {
    WRITE_ONCE(dev->replay_finished, true);
    wake_up_interruptible(&dev->wq_replay_done);
    notify_readers(dev);
}
```

### 3. Define ops callback functions

Write static functions that cast `void *data` to `echo_device*` and route
to the correct subsystem.  For example:

```c
/* Buffer ops */
static int buf_op_move_servo(void *data, u8 id, u16 angle) {
    return echo_servo_set_angle(((struct echo_device *)data)->servo, id, angle);
}
static bool buf_op_should_stop(void *data) {
    return echo_state_get_mode(((struct echo_device *)data)->state) != REPLAY;
}
```

Similarly for state_ops (7 callbacks) and joy_ops (2 callbacks).

### 4. Init sequence (with goto cleanup)

```c
static int __init echo_init(void)
{
    echo_dev = kzalloc(sizeof(*echo_dev), GFP_KERNEL);
    init_waitqueue_head(&echo_dev->wq_read);
    init_waitqueue_head(&echo_dev->wq_replay_done);

    echo_dev->servo    = echo_servo_create(sim_mode);         // or goto err_free
    echo_dev->buffer   = echo_buffer_create(&buf_ops, dev);   // or goto err_servo
    echo_dev->state    = echo_state_create(ms, &state_ops, dev); // or goto err_buffer
    echo_dev->joystick = echo_joystick_create(pins, sim, &joy_ops, dev); // ...
    echo_dev->chardev  = echo_chardev_create(dev);
    echo_dev->proc     = echo_proc_create(dev);
    return 0;

err_chardev: echo_chardev_destroy(dev->chardev);
err_joystick: echo_joystick_destroy(dev->joystick);
/* ... reverse chain ... */
err_free: kfree(echo_dev); return -ENODEV;
}
```

### 5. Exit (reverse order)

```c
static void __exit echo_exit(void)
{
    echo_proc_destroy(dev->proc);
    echo_chardev_destroy(dev->chardev);
    echo_joystick_destroy(dev->joystick);
    echo_state_destroy(dev->state);
    echo_buffer_destroy(dev->buffer);
    echo_servo_destroy(dev->servo);
    kfree(echo_dev);
}
```

---

## Step 11: Write the Makefile

```makefile
obj-m := echo_robot.o
echo_robot-objs := echo_main.o echo_chardev.o echo_servo.o \
                   echo_joystick.o echo_state.o echo_buffer.o echo_proc.o
ccflags-y := -Wall -Wextra
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

The `obj-m` line tells Kbuild to produce `echo_robot.ko`.
The `echo_robot-objs` line lists the `.o` files to link into it.

---

## Step 12: Build the User-Space App

### echo_app.c (main)

1. Open `/dev/echo_robot` with `O_RDWR`
2. Install SIGINT handler (sets `running = 0`)
3. `pthread_create` visualizer thread
4. `pthread_create` controller thread
5. `pthread_join` both
6. `close(fd)`

### echo_visualizer.c (Thread 1)

Loop while `running`:
1. `read(fd, &snap, sizeof(snap))` — blocks until kernel wakes wq_read
2. Render snap fields with ncurses

### echo_controller.c (Thread 2)

Loop while `running`:
1. Print menu, read stdin choice
2. Build `echo_cmd`, call `write(fd, &cmd, sizeof(cmd))`
3. Or call `ioctl(fd, ECHO_IOC_*, &arg)`

### App Makefile

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -lncurses -lpthread
TARGET = echo_app
SRCS = echo_app.c echo_visualizer.c echo_controller.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f $(OBJS) $(TARGET)
```

---

## Step 13: Write Helper Scripts

### load_module.sh

```bash
#!/bin/bash
set -e
# Check root, check .ko exists
# Unload if already loaded: lsmod | grep echo_robot && rmmod echo_robot
# insmod module/echo_robot.ko "$@"
# Wait 1s for udev, fallback to mknod if needed
# chmod 666 /dev/echo_robot
```

### unload_module.sh

```bash
#!/bin/bash
set -e
rmmod echo_robot
rm -f /dev/echo_robot   # in case manually created
```

### test_blocking.sh

See TESTING.md for the full test suite.

---

## Key Design Decisions Summary

| Decision | Reasoning |
|----------|-----------|
| Callback ops pattern | Prevents circular #includes between subsystems |
| Opaque `*_ctx` pointers | Encapsulation — subsystem internals are private |
| Coordinator in echo_main.c | Single place for all cross-module wiring |
| spinlock for mode | Timer callback runs in atomic (softirq) context |
| mutex for servo | I2C transfers can sleep |
| kfifo (256) | Power-of-2 required by kernel kfifo API |
| Snapshot/restore in replay | Preserves recording for repeated replays |
| Threaded IRQ | Thread function can sleep (needed for I2C downstream) |
| WRITE_ONCE/READ_ONCE on bools | Prevents compiler from caching across wait_event |
| Single workqueue | Only one replay can run at a time |
| sim_mode default=true | Development works on any Linux machine |
| echo_ioctl.h shared header | Single source of truth for kernel/user structs |

---

## Gotchas and Pitfalls

1. **Do not call cancel_work_sync from inside the worker.**
   `echo_state_replay_complete()` only sets mode=IDLE.  It does NOT call
   cancel_replay, because the replay worker calling cancel_work_sync on
   itself would deadlock.

2. **kfifo size must be power of 2.**  The kernel kfifo API requires this.
   256 is a good default.

3. **No floating point in kernel space.**  The angle-to-PWM conversion uses
   integer arithmetic only: `205 + (angle * 205) / 180`.

4. **spin_lock_irqsave, not spin_lock.**  The mode_lock might be taken from
   both process context and timer callback (softirq).  Always use irqsave
   variant to avoid deadlocks.

5. **IS_ERR / ERR_PTR pattern.**  Subsystem create functions return
   `ERR_PTR(-errno)` on failure, not NULL.  Check with `IS_ERR()`.

6. **Goto-chain cleanup.**  If step N fails, clean up steps N-1 through 1
   in reverse.  This is the standard Linux kernel error handling pattern.

7. **Module parameter permissions.**  0444 = read-only after load.
   0644 = can be changed at runtime via sysfs.

8. **copy_from_user / copy_to_user can fail.**  Always check the return
   value and return `-EFAULT` on failure.

9. **wait_event_interruptible returns non-zero on signal.**  Always check
   and return `-ERESTARTSYS` so the syscall can be restarted.
