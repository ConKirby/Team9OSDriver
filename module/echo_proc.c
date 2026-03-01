// SPDX-License-Identifier: GPL-2.0
/*
 * echo_proc.c — /proc/echo_stats interface for Project Echo
 *
 * Exposes a read-only /proc entry using the seq_file single-open helper.
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "echo_main.h"
#include "echo_proc.h"
#include "echo_buffer.h"

/* Persistent handle so cleanup can remove the entry */
static struct proc_dir_entry *echo_proc_entry;

/* ------------------------------------------------------------------ */
/*  seq_file show callback                                            */
/* ------------------------------------------------------------------ */

static int echo_proc_show(struct seq_file *m, void *v)
{
	struct echo_device *dev = echo_dev;
	enum echo_mode cur_mode;
	u16 pan, tilt;
	unsigned int buf_count;
	int total_moves, total_replays, irq_count, open_cnt;
	unsigned long flags;
	const char *mode_str;

	/* Mode — protected by spinlock */
	spin_lock_irqsave(&dev->mode_lock, flags);
	cur_mode = dev->mode;
	spin_unlock_irqrestore(&dev->mode_lock, flags);

	switch (cur_mode) {
	case ECHO_MODE_TEACH:
		mode_str = "TEACH";
		break;
	case ECHO_MODE_REPLAY:
		mode_str = "REPLAY";
		break;
	default:
		mode_str = "IDLE";
		break;
	}

	/* Servo positions — protected by mutex */
	mutex_lock(&dev->servo_lock);
	pan  = dev->servo_pos[ECHO_SERVO_PAN];
	tilt = dev->servo_pos[ECHO_SERVO_TILT];
	mutex_unlock(&dev->servo_lock);

	/* Buffer usage */
	buf_count = echo_buffer_count(dev);

	/* Atomic stats */
	total_moves   = atomic_read(&dev->stat_total_moves);
	total_replays = atomic_read(&dev->stat_replays);
	irq_count     = atomic_read(&dev->stat_irq_count);

	/* Open count — protected by mutex */
	mutex_lock(&dev->open_lock);
	open_cnt = dev->open_count;
	mutex_unlock(&dev->open_lock);

	seq_puts(m, "=== Project Echo Stats ===\n");
	seq_printf(m, "Mode:           %s\n", mode_str);
	seq_printf(m, "Pan Angle:      %u\n", pan);
	seq_printf(m, "Tilt Angle:     %u\n", tilt);
	seq_printf(m, "Buffer Used:    %u / %u\n", buf_count, ECHO_FIFO_SIZE);
	seq_printf(m, "Total Moves:    %d\n", total_moves);
	seq_printf(m, "Total Replays:  %d\n", total_replays);
	seq_printf(m, "IRQ Count:      %d\n", irq_count);
	seq_printf(m, "Open Count:     %d\n", open_cnt);
	seq_printf(m, "Sim Mode:       %s\n", dev->sim_mode ? "yes" : "no");
	seq_printf(m, "Timeout:        %lu ms\n", dev->timeout_ms);

	return 0;
}

/* ------------------------------------------------------------------ */
/*  proc_ops callbacks                                                */
/* ------------------------------------------------------------------ */

static int echo_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, echo_proc_show, NULL);
}

static const struct proc_ops echo_proc_ops = {
	.proc_open    = echo_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------ */
/*  init / cleanup                                                    */
/* ------------------------------------------------------------------ */

int echo_proc_init(struct echo_device *dev)
{
	echo_proc_entry = proc_create("echo_stats", 0444, NULL,
				      &echo_proc_ops);
	if (!echo_proc_entry) {
		pr_err("echo: failed to create /proc/echo_stats\n");
		return -ENOMEM;
	}

	pr_info("echo: /proc/echo_stats created\n");
	return 0;
}

void echo_proc_cleanup(struct echo_device *dev)
{
	remove_proc_entry("echo_stats", NULL);
}
