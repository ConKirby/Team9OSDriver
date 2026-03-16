/*echo_buffer.c — kfifo ring buffer + replay workqueue
 * Stores recorded servo movements.  The replay worker plays them back
 * from a snapshot so the original recording is preserved for repeats.
 * Dependencies: echo_buffer_ops callbacks only (wired by echo_main.c).
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "echo_types.h"
#include "echo_buffer.h"

// private context
struct echo_buffer_ctx {
	DECLARE_KFIFO(move_fifo, struct echo_move, ECHO_FIFO_SIZE);
	spinlock_t fifo_lock;

	struct workqueue_struct *replay_wq;
	struct work_struct       replay_work;
	u16 replay_speed;

	const struct echo_buffer_ops *ops;
	void *ops_data;

	atomic_t replay_count;
};

//replay worker

static void replay_worker(struct work_struct *work)
{
	struct echo_buffer_ctx *ctx =
		container_of(work, struct echo_buffer_ctx, replay_work);
	struct echo_move *moves;
	unsigned int count, i;
	unsigned long delay;

	moves = kmalloc_array(ECHO_FIFO_SIZE, sizeof(*moves), GFP_KERNEL);
	if (!moves) {
		pr_err("echo: buffer: replay OOM\n");
		ctx->ops->replay_done(ctx->ops_data);
		return;
	}

	/*
	 * Snapshot: drain all entries into a local array,
	 * then push them back so the recording is preserved.
	 */
	spin_lock(&ctx->fifo_lock);
	count = kfifo_len(&ctx->move_fifo);
	if (count > ECHO_FIFO_SIZE)
		count = ECHO_FIFO_SIZE;
	for (i = 0; i < count; i++)
		if (!kfifo_get(&ctx->move_fifo, &moves[i]))
    		break;
	for (i = 0; i < count; i++)
		kfifo_put(&ctx->move_fifo, moves[i]);
	spin_unlock(&ctx->fifo_lock);

	atomic_inc(&ctx->replay_count);
	pr_info("echo: buffer: replay started (%u moves)\n", count);

	for (i = 0; i < count; i++) {
		if (ctx->ops->should_stop(ctx->ops_data))
			break;

		delay = moves[i].delay_ms;
		if (ctx->replay_speed > 0)
			delay /= ctx->replay_speed;
		if (delay < 10)
			delay = 10;

		msleep(delay);

		ctx->ops->move_servo(ctx->ops_data,
				     moves[i].servo_id,
				     moves[i].angle);
		ctx->ops->notify(ctx->ops_data);
	}

	pr_info("echo: buffer: replay finished\n");
	ctx->ops->replay_done(ctx->ops_data);

	kfree(moves);
}

//public API

struct echo_buffer_ctx *echo_buffer_create(const struct echo_buffer_ops *ops,
					   void *ops_data)
{
	struct echo_buffer_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_KFIFO(ctx->move_fifo);
	spin_lock_init(&ctx->fifo_lock);

	ctx->ops      = ops;
	ctx->ops_data = ops_data;
	ctx->replay_speed = 1;

	ctx->replay_wq = create_singlethread_workqueue("echo_replay_wq");
	if (!ctx->replay_wq) {
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	INIT_WORK(&ctx->replay_work, replay_worker);
	atomic_set(&ctx->replay_count, 0);

	pr_info("echo: buffer: subsystem created\n");
	return ctx;
}

    void echo_buffer_destroy(struct echo_buffer_ctx *ctx)
        {
        if (!ctx)
            return;
        cancel_work_sync(&ctx->replay_work);
        if (ctx->replay_wq)
            destroy_workqueue(ctx->replay_wq);
        kfree(ctx);
    }

int echo_buffer_record(struct echo_buffer_ctx *ctx,
		       const struct echo_move *move)
{
	spin_lock(&ctx->fifo_lock);
	if (kfifo_is_full(&ctx->move_fifo)) {
		pr_warn("echo: buffer: full, dropping oldest move\n");
		kfifo_skip(&ctx->move_fifo);
	}
	kfifo_put(&ctx->move_fifo, *move);
	spin_unlock(&ctx->fifo_lock);
	return 0;
}

void echo_buffer_clear(struct echo_buffer_ctx *ctx)
{
	spin_lock(&ctx->fifo_lock);
	kfifo_reset(&ctx->move_fifo);
	spin_unlock(&ctx->fifo_lock);
}

unsigned int echo_buffer_count(struct echo_buffer_ctx *ctx)
{
	return kfifo_len(&ctx->move_fifo);
}

void echo_buffer_start_replay(struct echo_buffer_ctx *ctx)
{
	queue_work(ctx->replay_wq, &ctx->replay_work);
}

void echo_buffer_cancel_replay(struct echo_buffer_ctx *ctx)
{
	cancel_work_sync(&ctx->replay_work);
}

void echo_buffer_set_replay_speed(struct echo_buffer_ctx *ctx, u16 speed)
{
	if (speed > 0)
		WRITE_ONCE(ctx->replay_speed, speed);
}

int echo_buffer_get_replay_count(struct echo_buffer_ctx *ctx)
{
	return atomic_read(&ctx->replay_count);
}