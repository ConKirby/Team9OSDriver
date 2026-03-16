// SPDX-License-Identifier: GPL-2.0
/*
 * echo_proc.c — /proc/echo_stats read-only statistics interface
 *
 * Uses seq_file single-open helper.  Queries all subsystem contexts
 * through echo_device back-pointer.
 *
 * Dependencies: echo_device (echo_main.h) → all subsystem public APIs.
 * 
 * This creates the read-only statistics interface for the user to monitor. Clened up obvious errors.
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "echo_main.h"
#include "echo_proc.h"
#include "echo_servo.h"
#include "echo_state.h"
#include "echo_buffer.h"
#include "echo_joystick.h"
#include "echo_chardev.h"

/* ── Private context ───────────────────────────────────────────────── */
struct echo_proc_ctx {
	struct proc_dir_entry *entry;
	struct echo_device    *dev;
};

/* ── seq_file show callback ────────────────────────────────────────── */

static int echo_proc_show(struct seq_file *m, void *v)
{
	struct echo_device *dev = m->private;
	enum echo_mode cur_mode;
	const char *mode_str;
	u16 pan, tilt, tilt2;
	unsigned int buf_count;
	int total_moves, total_replays, irq_count, open_cnt;

	cur_mode = echo_state_get_mode(dev->state);
	switch (cur_mode) {
	case ECHO_MODE_TEACH:  mode_str = "TEACH";  break;
	case ECHO_MODE_REPLAY: mode_str = "REPLAY"; break;
	default:               mode_str = "IDLE";   break;
	}

	pan   = echo_servo_get_angle(dev->servo, ECHO_SERVO_PAN);
	tilt  = echo_servo_get_angle(dev->servo, ECHO_SERVO_TILT);
	tilt2 = echo_servo_get_angle(dev->servo, ECHO_SERVO_TILT2);
	buf_count     = echo_buffer_count(dev->buffer);
	total_moves   = echo_state_get_total_moves(dev->state);
	total_replays = echo_buffer_get_replay_count(dev->buffer);
	irq_count     = echo_joystick_get_irq_count(dev->joystick);
	open_cnt      = echo_chardev_get_open_count(dev->chardev);

	seq_puts(m, "=== Project Echo Stats ===\n");
	seq_printf(m, "Mode:           %s\n", mode_str);
	seq_printf(m, "Pan Angle:      %u\n", pan);
	seq_printf(m, "Tilt Angle:     %u\n", tilt);
	seq_printf(m, "Tilt2 Angle:    %u\n", tilt2);
	seq_printf(m, "Buffer Used:    %u / %u\n", buf_count, ECHO_FIFO_SIZE);
	seq_printf(m, "Total Moves:    %d\n", total_moves);
	seq_printf(m, "Total Replays:  %d\n", total_replays);
	seq_printf(m, "IRQ Count:      %d\n", irq_count);
	seq_printf(m, "Open Count:     %d\n", open_cnt);
	seq_printf(m, "Sim Mode:       %s\n", dev->sim_mode ? "yes" : "no");
	seq_printf(m, "Timeout:        %lu ms\n", dev->timeout_ms);

	return 0;
}

/* ── proc_ops ──────────────────────────────────────────────────────── */

static int echo_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, echo_proc_show, pde_data(inode));
}

static const struct proc_ops echo_proc_ops = {
	.proc_open    = echo_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ── Public API ────────────────────────────────────────────────────── */

struct echo_proc_ctx *echo_proc_create(struct echo_device *dev)
{
	struct echo_proc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->dev = dev;
	ctx->entry = proc_create_data("echo_stats", 0444, NULL,
				      &echo_proc_ops, dev);
	if (!ctx->entry) {
		pr_err("echo: proc: failed to create /proc/echo_stats\n");
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	pr_info("echo: proc: /proc/echo_stats created\n");
	return ctx;
}

void echo_proc_destroy(struct echo_proc_ctx *ctx)
{
	if (!ctx)
		return;
	remove_proc_entry("echo_stats", NULL);
	kfree(ctx);
}