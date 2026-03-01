// SPDX-License-Identifier: GPL-2.0
/*
 * echo_buffer.c — kfifo circular buffer and replay worker
 *
 * Stores recorded servo movements in a lock-protected kfifo.
 * The replay worker plays them back from a snapshot so the
 * original recording is preserved for repeated replays.
 */

#include "echo_main.h"
#include "echo_buffer.h"
#include "echo_servo.h"

#include <linux/delay.h>
#include <linux/slab.h>

/* ---------- Init / Cleanup ---------- */

int echo_buffer_init(struct echo_device *dev)
{
	dev->replay_wq = create_singlethread_workqueue("echo_replay_wq");
	if (!dev->replay_wq)
		return -ENOMEM;

	INIT_WORK(&dev->replay_work, echo_buffer_replay_worker);
	pr_info("echo: Buffer subsystem initialized\n");
	return 0;
}

void echo_buffer_cleanup(struct echo_device *dev)
{
	cancel_work_sync(&dev->replay_work);
	if (dev->replay_wq)
		destroy_workqueue(dev->replay_wq);
	pr_info("echo: Buffer subsystem cleaned up\n");
}

/* ---------- Record / Read / Clear ---------- */

int echo_buffer_record(struct echo_device *dev, const struct echo_move *move)
{
	spin_lock(&dev->fifo_lock);

	/* If the FIFO is full, drop the oldest entry to make room */
	if (kfifo_is_full(&dev->move_fifo))
		kfifo_skip(&dev->move_fifo);

	kfifo_put(&dev->move_fifo, *move);

	spin_unlock(&dev->fifo_lock);
	return 0;
}

int echo_buffer_get_next(struct echo_device *dev, struct echo_move *move)
{
	int ret;

	spin_lock(&dev->fifo_lock);
	ret = kfifo_get(&dev->move_fifo, move);
	spin_unlock(&dev->fifo_lock);

	return ret ? 0 : -ENODATA;
}

void echo_buffer_clear(struct echo_device *dev)
{
	spin_lock(&dev->fifo_lock);
	kfifo_reset(&dev->move_fifo);
	spin_unlock(&dev->fifo_lock);
}

unsigned int echo_buffer_count(struct echo_device *dev)
{
	/* Lock-free read is acceptable for an approximate count */
	return kfifo_len(&dev->move_fifo);
}

/* ---------- Replay worker ---------- */

void echo_buffer_replay_worker(struct work_struct *work)
{
	struct echo_device *dev = container_of(work, struct echo_device,
					       replay_work);
	struct echo_move *moves;
	unsigned int count, i;
	enum echo_mode cur_mode;
	unsigned long delay;

	moves = kmalloc_array(ECHO_FIFO_SIZE, sizeof(*moves), GFP_KERNEL);
	if (!moves) {
		pr_err("echo: Replay failed — out of memory\n");
		dev->replay_finished = true;
		wake_up_interruptible(&dev->wq_replay_done);
		return;
	}

	/*
	 * Snapshot the FIFO: drain all entries into a local array,
	 * then push them back so the recording is preserved.
	 */
	spin_lock(&dev->fifo_lock);
	count = kfifo_len(&dev->move_fifo);
	if (count > ECHO_FIFO_SIZE)
		count = ECHO_FIFO_SIZE;
	for (i = 0; i < count; i++)
		kfifo_get(&dev->move_fifo, &moves[i]);
	/* Restore the entries back into the FIFO */
	for (i = 0; i < count; i++)
		kfifo_put(&dev->move_fifo, moves[i]);
	spin_unlock(&dev->fifo_lock);

	atomic_inc(&dev->stat_replays);
	pr_info("echo: Replay started (%u moves)\n", count);

	for (i = 0; i < count; i++) {
		/* Check whether we are still in REPLAY mode */
		spin_lock(&dev->mode_lock);
		cur_mode = dev->mode;
		spin_unlock(&dev->mode_lock);

		if (cur_mode != ECHO_MODE_REPLAY)
			break;

		/* Calculate delay, respecting replay speed multiplier */
		delay = moves[i].delay_ms;
		if (dev->replay_speed > 0)
			delay /= dev->replay_speed;
		if (delay < 10)
			delay = 10;

		msleep(delay);

		echo_servo_set_angle(dev, moves[i].servo_id, moves[i].angle);

		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
	}

	/* Transition back to idle */
	spin_lock(&dev->mode_lock);
	dev->mode = ECHO_MODE_IDLE;
	spin_unlock(&dev->mode_lock);

	dev->replay_finished = true;
	wake_up_interruptible(&dev->wq_replay_done);

	dev->new_data_avail = true;
	wake_up_interruptible(&dev->wq_read);

	pr_info("echo: Replay finished\n");
	kfree(moves);
}
