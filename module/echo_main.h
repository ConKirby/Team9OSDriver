/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_MAIN_H
#define ECHO_MAIN_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define ECHO_DEVICE_NAME   "echo_robot"
#define ECHO_CLASS_NAME    "echo"
#define ECHO_FIFO_SIZE     256

#define ECHO_NUM_SERVOS    2
#define ECHO_SERVO_PAN     0
#define ECHO_SERVO_TILT    1

#define ECHO_NUM_GPIO      5
#define ECHO_GPIO_UP       0
#define ECHO_GPIO_DOWN     1
#define ECHO_GPIO_LEFT     2
#define ECHO_GPIO_RIGHT    3
#define ECHO_GPIO_BUTTON   4

#define ECHO_DEFAULT_TIMEOUT_MS  5000
#define ECHO_SERVO_CENTER        90
#define ECHO_SERVO_MIN           0
#define ECHO_SERVO_MAX           180
#define ECHO_DEBOUNCE_MS         50
#define ECHO_REPLAY_STEP_MS      50
#define ECHO_ANGLE_STEP          5

/* Operating modes */
enum echo_mode {
	ECHO_MODE_IDLE,
	ECHO_MODE_TEACH,
	ECHO_MODE_REPLAY,
};

/* A single recorded servo movement */
struct echo_move {
	u8 servo_id;    /* ECHO_SERVO_PAN or ECHO_SERVO_TILT */
	u16 angle;      /* 0-180 degrees */
	u32 delay_ms;   /* time since previous move */
};

/* Write commands from user-space */
struct echo_cmd {
	u32 command;
	u32 servo_id;
	u32 angle;
	u32 speed;
};

#define ECHO_CMD_TEACH   1
#define ECHO_CMD_REPLAY  2
#define ECHO_CMD_STOP    3
#define ECHO_CMD_MOVE    4

/* State snapshot returned via read() */
struct echo_state_snapshot {
	u32 mode;
	u16 pan_angle;
	u16 tilt_angle;
	u32 buffer_count;
	u32 total_moves;
	u32 total_replays;
};

/* Master device structure */
struct echo_device {
	/* Char device */
	struct cdev cdev;
	dev_t devno;
	struct class *dev_class;
	struct device *device;
	int open_count;
	struct mutex open_lock;

	/* I2C / Servo */
	struct i2c_adapter *i2c_adapter;
	struct i2c_client *i2c_client;
	bool sim_mode;
	u16 servo_pos[ECHO_NUM_SERVOS];
	struct mutex servo_lock;

	/* GPIO / Joystick */
	int gpio_pins[ECHO_NUM_GPIO];
	int irqs[ECHO_NUM_GPIO];
	unsigned long last_irq_jiffies[ECHO_NUM_GPIO];

	/* State Machine */
	enum echo_mode mode;
	spinlock_t mode_lock;
	struct timer_list inactivity_timer;
	unsigned long timeout_ms;
	u16 replay_speed;

	/* Command Buffer */
	DECLARE_KFIFO(move_fifo, struct echo_move, ECHO_FIFO_SIZE);
	spinlock_t fifo_lock;
	ktime_t last_move_time;

	/* Wait Queues */
	wait_queue_head_t wq_read;
	wait_queue_head_t wq_replay_done;
	bool new_data_avail;
	bool replay_finished;

	/* Replay Workqueue */
	struct workqueue_struct *replay_wq;
	struct work_struct replay_work;

	/* /proc stats */
	atomic_t stat_total_moves;
	atomic_t stat_replays;
	atomic_t stat_irq_count;
};

/* Global device pointer (single-instance driver) */
extern struct echo_device *echo_dev;

#endif /* ECHO_MAIN_H */
