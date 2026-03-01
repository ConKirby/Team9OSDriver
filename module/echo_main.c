// SPDX-License-Identifier: GPL-2.0
/*
 * echo_main.c - Module entry point for Project Echo Pan/Tilt Servo Driver
 *
 * Initialises the master echo_device structure and calls each subsystem
 * init in dependency order.  Teardown happens in the reverse order.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "echo_main.h"
#include "echo_chardev.h"
#include "echo_servo.h"
#include "echo_joystick.h"
#include "echo_state.h"
#include "echo_buffer.h"
#include "echo_proc.h"

/* ── Global device pointer (single-instance driver) ────────────────── */
struct echo_device *echo_dev;

/* ── Module parameters ─────────────────────────────────────────────── */
static int gpio_up     = 17;
static int gpio_down   = 27;
static int gpio_left   = 22;
static int gpio_right  = 23;
static int gpio_button = 24;

module_param(gpio_up,     int, 0444);
MODULE_PARM_DESC(gpio_up,     "GPIO pin for UP direction");

module_param(gpio_down,   int, 0444);
MODULE_PARM_DESC(gpio_down,   "GPIO pin for DOWN direction");

module_param(gpio_left,   int, 0444);
MODULE_PARM_DESC(gpio_left,   "GPIO pin for LEFT direction");

module_param(gpio_right,  int, 0444);
MODULE_PARM_DESC(gpio_right,  "GPIO pin for RIGHT direction");

module_param(gpio_button, int, 0444);
MODULE_PARM_DESC(gpio_button, "GPIO pin for BUTTON input");

static unsigned long timeout_ms = ECHO_DEFAULT_TIMEOUT_MS;
module_param(timeout_ms, ulong, 0644);
MODULE_PARM_DESC(timeout_ms, "Inactivity timeout in milliseconds");

static bool sim_mode = true;
module_param(sim_mode, bool, 0444);
MODULE_PARM_DESC(sim_mode, "Simulation mode (no real hardware)");

/* ── Module init ───────────────────────────────────────────────────── */
static int __init echo_init(void)
{
	int ret;

	/* 1. Allocate the master device structure */
	echo_dev = kzalloc(sizeof(*echo_dev), GFP_KERNEL);
	if (!echo_dev)
		return -ENOMEM;

	/* 2. Initialise all locks */
	mutex_init(&echo_dev->open_lock);
	mutex_init(&echo_dev->servo_lock);
	spin_lock_init(&echo_dev->mode_lock);
	spin_lock_init(&echo_dev->fifo_lock);

	/* 3. Initialise wait queues */
	init_waitqueue_head(&echo_dev->wq_read);
	init_waitqueue_head(&echo_dev->wq_replay_done);

	/* 4. Initialise the kfifo */
	INIT_KFIFO(echo_dev->move_fifo);

	/* 5. Default values */
	echo_dev->mode         = ECHO_MODE_IDLE;
	echo_dev->timeout_ms   = timeout_ms;
	echo_dev->sim_mode     = sim_mode;
	echo_dev->servo_pos[ECHO_SERVO_PAN]  = ECHO_SERVO_CENTER;
	echo_dev->servo_pos[ECHO_SERVO_TILT] = ECHO_SERVO_CENTER;
	echo_dev->replay_speed = 1;

	/* 6. Copy GPIO pins from module parameters */
	echo_dev->gpio_pins[ECHO_GPIO_UP]     = gpio_up;
	echo_dev->gpio_pins[ECHO_GPIO_DOWN]   = gpio_down;
	echo_dev->gpio_pins[ECHO_GPIO_LEFT]   = gpio_left;
	echo_dev->gpio_pins[ECHO_GPIO_RIGHT]  = gpio_right;
	echo_dev->gpio_pins[ECHO_GPIO_BUTTON] = gpio_button;

	/* 7. Zero out atomic stats */
	atomic_set(&echo_dev->stat_total_moves, 0);
	atomic_set(&echo_dev->stat_replays, 0);
	atomic_set(&echo_dev->stat_irq_count, 0);

	/* 8. Subsystem initialisation (order matters) */
	ret = echo_servo_init(echo_dev);
	if (ret)
		goto err_free;

	ret = echo_buffer_init(echo_dev);
	if (ret)
		goto err_servo;

	ret = echo_state_init(echo_dev);
	if (ret)
		goto err_buffer;

	ret = echo_joystick_init(echo_dev);
	if (ret)
		goto err_state;

	ret = echo_chardev_init(echo_dev);
	if (ret)
		goto err_joystick;

	ret = echo_proc_init(echo_dev);
	if (ret)
		goto err_chardev;

	/* 9. All good */
	pr_info("echo_robot: module loaded (sim_mode=%d, timeout=%lu ms)\n",
		echo_dev->sim_mode, echo_dev->timeout_ms);
	return 0;

err_chardev:
	echo_chardev_cleanup(echo_dev);
err_joystick:
	echo_joystick_cleanup(echo_dev);
err_state:
	echo_state_cleanup(echo_dev);
err_buffer:
	echo_buffer_cleanup(echo_dev);
err_servo:
	echo_servo_cleanup(echo_dev);
err_free:
	kfree(echo_dev);
	echo_dev = NULL;
	return ret;
}

/* ── Module exit ───────────────────────────────────────────────────── */
static void __exit echo_exit(void)
{
	/* Tear down in reverse init order */
	echo_proc_cleanup(echo_dev);
	echo_chardev_cleanup(echo_dev);
	echo_joystick_cleanup(echo_dev);
	echo_state_cleanup(echo_dev);
	echo_buffer_cleanup(echo_dev);
	echo_servo_cleanup(echo_dev);

	kfree(echo_dev);
	echo_dev = NULL;

	pr_info("echo_robot: module unloaded\n");
}

module_init(echo_init);
module_exit(echo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Team 9");
MODULE_DESCRIPTION("Project Echo - Pan/Tilt Servo Driver");
