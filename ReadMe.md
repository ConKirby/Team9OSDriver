# Project Echo — Linux Kernel Module Device Driver

A loadable kernel module (LKM) character device driver for Raspberry Pi 5 that controls a pan/tilt servo mount via a PCA9685 PWM controller over I2C, driven by a GPIO joystick with interrupt handling.

Built by **Team 9** for the ISE Block 3 Operating Systems assignment.

---

## How We Used Claude

This project was developed with the assistance of **Claude Code** (Anthropic's CLI agent) to accelerate design, implementation, and code review. Here is how Claude was involved at each stage and what we learned from the process.

### 1. Architecture and Planning

We described our hardware setup (Raspberry Pi 5, PCA9685, joystick on GPIO) and the assignment requirements to Claude. It helped us design the full system architecture:

- Splitting the module into logical subsystems (chardev, servo, joystick, state machine, buffer, proc)
- Choosing between implementation strategies (e.g. `workqueue` vs `kthread` for replay, `kfifo` vs linked list for buffering)
- Defining the API contracts between subsystems as header files before writing any implementation
- Planning the blocking I/O design with wait queues

This planning phase was critical. By having Claude generate the full set of header files with function signatures first, we established clear interfaces that let all team members work on their subsystems independently.

### 2. Code Generation

With the architecture locked in, we used Claude to generate the initial implementation of each subsystem in parallel:

- **Kernel module** (`module/`): 7 source files compiled into a single `echo_robot.ko`
- **User-space app** (`app/`): Multi-threaded ncurses application with blocking read/write
- **Scripts** (`scripts/`): Load/unload helpers and a stress test suite
- **Documentation** (`docs/`): Design document and testing guide

Claude produced code that follows kernel coding conventions — proper `SPDX` license headers, `goto`-chain error cleanup in init functions, correct locking discipline with mutexes and spinlocks, and `request_threaded_irq` for handlers that need to sleep.

### 3. Automated Code Review

After generation, Claude performed a cross-file consistency review and caught several issues before we ever tried to compile:

| Issue | Severity | Fix |
|-------|----------|-----|
| PCA9685 MODE2 register address was `0x04` instead of `0x01` | Medium | Corrected to match datasheet |
| `echo_servo_get_angle()` had no bounds check on `servo_id` | Medium | Added validation |
| `echo_servo_get_angle()` read shared state without holding the mutex | Medium | Added `servo_lock` protection |
| Replay worker allocated a 2KB array on the kernel stack | Low | Changed to `kmalloc` |
| Dead code: clamping after an early return in `set_angle()` | Low | Removed redundant code |
| Missing `#include <string.h>` in user-space app | Low | Added the include |

This review step demonstrated how AI tooling can catch bugs that would otherwise only surface during testing or, worse, in production as kernel panics.

### 4. What Claude Demonstrated About the LKM

By generating a complete, working implementation, Claude helped us understand and demonstrate the following kernel concepts:

**Character Device Interface**
- Registering a device with `alloc_chrdev_region`, `cdev_init`, `class_create`, `device_create`
- Implementing all required `file_operations`: `open`, `close`, `read`, `write`, `unlocked_ioctl`
- Clean teardown in reverse order with NULL checks

**Blocking I/O (Primary Assessment Criteria)**
- `read()` blocks using `wait_event_interruptible()` on a wait queue until new data arrives
- `write(REPLAY)` blocks until the replay workqueue finishes draining the buffer
- `O_NONBLOCK` support returning `-EAGAIN` when no data is available
- Signal handling returning `-ERESTARTSYS` for interrupted waits

**Interrupt Handling**
- `request_threaded_irq()` with a hard-IRQ top half that returns `IRQ_WAKE_THREAD` and a threaded bottom half that can safely call I2C functions
- Software debouncing using `jiffies` comparison (50ms minimum interval)
- `IRQF_TRIGGER_FALLING | IRQF_ONESHOT` flags for GPIO button edges

**Concurrency and Synchronisation**
- `mutex` for sleeping contexts (servo I2C access, open count)
- `spinlock` for atomic contexts (mode transitions in timer callbacks, kfifo access)
- `wait_queue_head_t` for blocking I/O between kernel and user-space
- `atomic_t` for lock-free statistics counters

**Kernel Timers and Work Queues**
- `timer_setup` + `mod_timer` for the inactivity timeout that triggers auto-replay
- `create_singlethread_workqueue` + `INIT_WORK` for the replay worker
- `cancel_work_sync` for safe cleanup during module unload

**Hardware Abstraction**
- `i2c_get_adapter` + `i2c_new_client_device` for runtime I2C device creation (no device tree changes needed)
- PCA9685 PWM register programming (prescale for 50Hz, per-channel ON/OFF ticks)
- Simulation mode fallback that logs operations without hardware access

**/proc Filesystem**
- `proc_create` with `seq_file` interface for `/proc/echo_stats`
- Proper locking when reading device state for the proc output

### 5. What We Learned

Working with Claude on this project taught us several things:

- **Interface-first design pays off.** Defining headers before implementations forced us to think about API boundaries, which made the code modular and testable.
- **AI-generated code still needs review.** Claude produced a wrong register address from the PCA9685 datasheet and missed a data race. Always verify hardware details against datasheets and review locking discipline.
- **Understanding is non-negotiable.** Every team member studied and can explain all the generated code. The AI accelerated implementation but did not replace the need to understand `wait_event_interruptible`, spinlock vs mutex, or why `kfifo` is appropriate for IRQ contexts.
- **Simulation mode enables parallel development.** Having the module work without hardware (via `sim_mode=1`) meant we could develop and test the blocking I/O, state machine, and user-space app on any Linux machine while the hardware integration was done separately on the Pi.

---

## Quick Start

```bash
# Build the kernel module (on Raspberry Pi 5 or Linux with kernel headers)
cd module
make

# Load in simulation mode (no hardware needed)
sudo ../scripts/load_module.sh

# Verify it loaded
cat /proc/echo_stats
dmesg | tail -20

# Build and run the user-space app
cd ../app
make
sudo ./echo_app

# Unload when done
sudo ../scripts/unload_module.sh
```

---

## Repository Structure

```
Team9OSDriver/
├── module/              # Kernel module (builds echo_robot.ko)
│   ├── echo_main.c/h       Module init/exit, device struct
│   ├── echo_chardev.c/h    Character device file_operations
│   ├── echo_servo.c/h      I2C + PCA9685 servo control
│   ├── echo_joystick.c/h   GPIO input, threaded IRQ handlers
│   ├── echo_state.c/h      State machine + inactivity timer
│   ├── echo_buffer.c/h     kfifo circular buffer + replay worker
│   ├── echo_proc.c/h       /proc/echo_stats interface
│   ├── echo_ioctl.h        ioctl commands (shared with user-space)
│   └── Makefile
├── app/                 # User-space application
│   ├── echo_app.c           Main: threads + signal handling
│   ├── echo_visualizer.c/h  Thread 1: ncurses UI, blocking read
│   ├── echo_controller.c/h  Thread 2: ioctl + blocking write
│   └── Makefile
├── scripts/             # Helper scripts
│   ├── load_module.sh       insmod + mknod
│   ├── unload_module.sh     rmmod cleanup
│   └── test_blocking.sh     Stress test suite
├── docs/                # Documentation
│   ├── DESIGN.md            Architecture and design decisions
│   └── TESTING.md           Test procedures and troubleshooting
└── Brief.md             # Assignment brief
```

---

## Team 9

ISE Block 3, 2026.
