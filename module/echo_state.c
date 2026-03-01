// SPDX-License-Identifier: GPL-2.0
/*
 * echo_state.c — State machine, inactivity timer, and mode transitions
 *
 * Manages IDLE / TEACH / REPLAY modes for the echo robot driver.
 * An inactivity timer automatically transitions from TEACH to REPLAY
 * when the joystick has been idle for timeout_ms milliseconds.
 */

#include "echo_main.h"
#include "echo_state.h"
#include "echo_servo.h"
#include "echo_buffer.h"

#include <linux/jiffies.h>

/* ---------- Inactivity timer callback ---------- */

static void inactivity_timer_callback(struct timer_list *t)
{
	struct echo_device *dev = container_of(t, struct echo_device,
					       inactivity_timer);

	spin_lock(&dev->mode_lock);
	if (dev->mode == ECHO_MODE_TEACH) {
		dev->mode = ECHO_MODE_REPLAY;
		spin_unlock(&dev->mode_lock);

		pr_info("echo: Inactivity timeout — starting replay\n");
		queue_work(dev->replay_wq, &dev->replay_work);

		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
	} else {
		spin_unlock(&dev->mode_lock);
	}
}

/* ---------- Init / Cleanup ---------- */

int echo_state_init(struct echo_device *dev)
{
	timer_setup(&dev->inactivity_timer, inactivity_timer_callback, 0);
	pr_info("echo: State machine initialized\n");
	return 0;
}

void echo_state_cleanup(struct echo_device *dev)
{
	del_timer_sync(&dev->inactivity_timer);
	pr_info("echo: State machine cleaned up\n");
}

/* ---------- Input handling (called from joystick threaded IRQ) ---------- */

void echo_state_handle_input(struct echo_device *dev, u8 servo_id, int delta)
{
	u16 cur_angle, new_angle;
	int tmp;
	struct echo_move move;
	ktime_t now;

	spin_lock(&dev->mode_lock);

	/* Ignore input while a replay is running */
	if (dev->mode == ECHO_MODE_REPLAY) {
		spin_unlock(&dev->mode_lock);
		return;
	}

	/* Auto-enter teach mode on first input from idle */
	if (dev->mode == ECHO_MODE_IDLE)
		dev->mode = ECHO_MODE_TEACH;

	spin_unlock(&dev->mode_lock);

	/* Calculate and clamp new angle */
	cur_angle = echo_servo_get_angle(dev, servo_id);
	tmp = (int)cur_angle + delta;
	if (tmp < ECHO_SERVO_MIN)
		tmp = ECHO_SERVO_MIN;
	if (tmp > ECHO_SERVO_MAX)
		tmp = ECHO_SERVO_MAX;
	new_angle = (u16)tmp;

	echo_servo_set_angle(dev, servo_id, new_angle);

	/* Record the move into the buffer */
	now = ktime_get();
	move.servo_id = servo_id;
	move.angle    = new_angle;
	move.delay_ms = (u32)ktime_ms_delta(now, dev->last_move_time);
	dev->last_move_time = now;

	echo_buffer_record(dev, &move);
	atomic_inc(&dev->stat_total_moves);

	/* Restart the inactivity timer */
	mod_timer(&dev->inactivity_timer,
		  jiffies + msecs_to_jiffies(dev->timeout_ms));

	/* Notify any blocking readers */
	dev->new_data_avail = true;
	wake_up_interruptible(&dev->wq_read);
}

/* ---------- Replay start ---------- */

int echo_state_start_replay(struct echo_device *dev)
{
	spin_lock(&dev->mode_lock);

	if (dev->mode == ECHO_MODE_REPLAY) {
		spin_unlock(&dev->mode_lock);
		return -EBUSY;
	}

	dev->mode = ECHO_MODE_REPLAY;
	spin_unlock(&dev->mode_lock);

	/* No inactivity timeout during replay */
	del_timer_sync(&dev->inactivity_timer);

	dev->replay_finished = false;
	queue_work(dev->replay_wq, &dev->replay_work);

	dev->new_data_avail = true;
	wake_up_interruptible(&dev->wq_read);
	return 0;
}

/* ---------- Stop (return to idle) ---------- */

void echo_state_stop(struct echo_device *dev)
{
	spin_lock(&dev->mode_lock);
	dev->mode = ECHO_MODE_IDLE;
	spin_unlock(&dev->mode_lock);

	del_timer_sync(&dev->inactivity_timer);
	cancel_work_sync(&dev->replay_work);

	dev->replay_finished = true;
	wake_up_interruptible(&dev->wq_replay_done);

	dev->new_data_avail = true;
	wake_up_interruptible(&dev->wq_read);
}

/* ---------- Explicit mode switch ---------- */

int echo_state_set_mode(struct echo_device *dev, enum echo_mode new_mode)
{
	/* Delegate to the specialised helpers for replay and idle */
	if (new_mode == ECHO_MODE_REPLAY)
		return echo_state_start_replay(dev);

	if (new_mode == ECHO_MODE_IDLE) {
		echo_state_stop(dev);
		return 0;
	}

	/* TEACH mode */
	spin_lock(&dev->mode_lock);
	dev->mode = new_mode;
	spin_unlock(&dev->mode_lock);

	if (new_mode == ECHO_MODE_TEACH) {
		dev->last_move_time = ktime_get();
		echo_buffer_clear(dev);
		mod_timer(&dev->inactivity_timer,
			  jiffies + msecs_to_jiffies(dev->timeout_ms));
	}

	dev->new_data_avail = true;
	wake_up_interruptible(&dev->wq_read);
	return 0;
}
