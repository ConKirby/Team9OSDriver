// SPDX-License-Identifier: GPL-2.0
/*
 * echo_state.c — State machine, inactivity timer, and mode transitions
 *
 * Manages IDLE / TEACH / REPLAY modes.  An inactivity timer
 * auto-transitions from TEACH to REPLAY after timeout_ms of silence.
 *
 * Dependencies: echo_state_ops callbacks only (wired by echo_main.c).
 */

#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/timer.h>

#include "echo_types.h"
#include "echo_state.h"

/* ── Private context ───────────────────────────────────────────────── */
struct echo_state_ctx {
	enum echo_mode mode;
	spinlock_t     mode_lock;	/* always taken with irqsave */

	struct timer_list inactivity_timer;
	unsigned long     timeout_ms;

	ktime_t last_move_time;
	seqlock_t time_lock;		/* protects last_move_time */

	const struct echo_state_ops *ops;
	void *ops_data;

	atomic_t total_moves;
};

/* ── Inactivity timer callback (runs in softirq — atomic context) ──── */

static void inactivity_timer_fn(struct timer_list *t)
{
	struct echo_state_ctx *ctx =
		timer_container_of(ctx, t, inactivity_timer);
	unsigned long flags;

	spin_lock_irqsave(&ctx->mode_lock, flags);
	if (ctx->mode != ECHO_MODE_TEACH) {
		spin_unlock_irqrestore(&ctx->mode_lock, flags);
		return;
	}
	ctx->mode = ECHO_MODE_REPLAY;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	pr_info("echo: state: inactivity timeout — starting replay\n");

	/* queue_work and wake_up are safe from atomic context */
	ctx->ops->start_replay(ctx->ops_data);
	ctx->ops->notify(ctx->ops_data);
}

/* ── Public API ────────────────────────────────────────────────────── */

struct echo_state_ctx *echo_state_create(unsigned long timeout_ms,
					 const struct echo_state_ops *ops,
					 void *ops_data)
{
	struct echo_state_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->mode       = ECHO_MODE_IDLE;
	ctx->timeout_ms = timeout_ms;
	ctx->ops        = ops;
	ctx->ops_data   = ops_data;

	spin_lock_init(&ctx->mode_lock);
	seqlock_init(&ctx->time_lock);
	timer_setup(&ctx->inactivity_timer, inactivity_timer_fn, 0);
	atomic_set(&ctx->total_moves, 0);

	pr_info("echo: state: subsystem created (timeout=%lu ms)\n",
		timeout_ms);
	return ctx;
}

void echo_state_destroy(struct echo_state_ctx *ctx)
{
	if (!ctx)
		return;
	timer_delete_sync(&ctx->inactivity_timer);
	kfree(ctx);
}

/*
 * echo_state_handle_input — process a joystick direction event
 *
 * Called from threaded IRQ context (can sleep).
 * Auto-enters TEACH mode from IDLE on first input.
 */
void echo_state_handle_input(struct echo_state_ctx *ctx,
			     u8 servo_id, int delta)
{
	unsigned long flags;
	u16 cur_angle, new_angle;
	int tmp;
	struct echo_move move;
	ktime_t now;

	spin_lock_irqsave(&ctx->mode_lock, flags);

	/* Ignore input during replay */
	if (ctx->mode == ECHO_MODE_REPLAY) {
		spin_unlock_irqrestore(&ctx->mode_lock, flags);
		return;
	}

	/* Auto-enter teach mode on first input from idle */
	if (ctx->mode == ECHO_MODE_IDLE) {
		ctx->mode = ECHO_MODE_TEACH;
		ctx->last_move_time = ktime_get();  /* BUG FIX: init time */
	}

	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	/* Calculate and clamp new angle */
	cur_angle = ctx->ops->get_servo(ctx->ops_data, servo_id);
	tmp = (int)cur_angle + delta;
	if (tmp < ECHO_SERVO_MIN) tmp = ECHO_SERVO_MIN;
	if (tmp > ECHO_SERVO_MAX) tmp = ECHO_SERVO_MAX;
	new_angle = (u16)tmp;

	ctx->ops->move_servo(ctx->ops_data, servo_id, new_angle);

	/* Record the move */
	now = ktime_get();
	move.servo_id = servo_id;
	move.angle    = new_angle;

	write_seqlock(&ctx->time_lock);
	move.delay_ms = (u32)ktime_ms_delta(now, ctx->last_move_time);
	ctx->last_move_time = now;
	write_sequnlock(&ctx->time_lock);

	ctx->ops->record_move(ctx->ops_data, &move);
	atomic_inc(&ctx->total_moves);

	/* Restart the inactivity timer */
	mod_timer(&ctx->inactivity_timer,
		  jiffies + msecs_to_jiffies(ctx->timeout_ms));

	ctx->ops->notify(ctx->ops_data);
}

/*
 * echo_state_start_replay — explicitly start replay (from process context)
 *
 * Called by chardev write(REPLAY) or ioctl SET_MODE(REPLAY).
 */
int echo_state_start_replay(struct echo_state_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->mode_lock, flags);
	if (ctx->mode == ECHO_MODE_REPLAY) {
		spin_unlock_irqrestore(&ctx->mode_lock, flags);
		return -EBUSY;
	}
	ctx->mode = ECHO_MODE_REPLAY;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	timer_delete_sync(&ctx->inactivity_timer);

	ctx->ops->start_replay(ctx->ops_data);
	ctx->ops->notify(ctx->ops_data);
	return 0;
}

/*
 * echo_state_stop — stop everything and return to IDLE (process context)
 */
void echo_state_stop(struct echo_state_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->mode_lock, flags);
	ctx->mode = ECHO_MODE_IDLE;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	timer_delete_sync(&ctx->inactivity_timer);
	ctx->ops->cancel_replay(ctx->ops_data);
	ctx->ops->notify(ctx->ops_data);
}

/*
 * echo_state_set_mode — explicit mode transition from user-space
 */
int echo_state_set_mode(struct echo_state_ctx *ctx, enum echo_mode new_mode)
{
	unsigned long flags;

	if (new_mode == ECHO_MODE_REPLAY)
		return echo_state_start_replay(ctx);

	if (new_mode == ECHO_MODE_IDLE) {
		echo_state_stop(ctx);
		return 0;
	}

	/* TEACH mode */
	spin_lock_irqsave(&ctx->mode_lock, flags);
	ctx->mode = ECHO_MODE_TEACH;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	ctx->last_move_time = ktime_get();
	ctx->ops->clear_buffer(ctx->ops_data);
	mod_timer(&ctx->inactivity_timer,
		  jiffies + msecs_to_jiffies(ctx->timeout_ms));

	ctx->ops->notify(ctx->ops_data);
	return 0;
}

enum echo_mode echo_state_get_mode(struct echo_state_ctx *ctx)
{
	enum echo_mode m;
	unsigned long flags;

	spin_lock_irqsave(&ctx->mode_lock, flags);
	m = ctx->mode;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);

	return m;
}

/*
 * echo_state_replay_complete — called by buffer when replay finishes
 *
 * Sets mode to IDLE without calling cancel_replay (we are inside
 * the replay worker, so cancel_work_sync would deadlock).
 */
void echo_state_replay_complete(struct echo_state_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->mode_lock, flags);
	ctx->mode = ECHO_MODE_IDLE;
	spin_unlock_irqrestore(&ctx->mode_lock, flags);
}

int echo_state_get_total_moves(struct echo_state_ctx *ctx)
{
	return atomic_read(&ctx->total_moves);
}
