// SPDX-License-Identifier: GPL-2.0
/*
 * echo_proc.c — /proc/echo_stats statistics interface
 *
 * This file implements a read-only entry in the Linux /proc filesystem
 * that allows users to inspect the internal state of the Echo robot
 * driver at runtime.
 *
 * When the module is loaded it creates:
 *
 *      /proc/echo_stats
 *
 * Reading this file (e.g. `cat /proc/echo_stats`) prints useful
 * debugging and monitoring information such as:
 *
 *   • current operating mode
 *   • servo angles
 *   • number of recorded movements
 *   • replay statistics
 *   • joystick interrupt count
 *   • device open count
 *
 * The implementation uses the Linux seq_file API, which is the
 * recommended way to generate formatted output for /proc files.
 *
 * seq_file handles buffering and large outputs safely, avoiding many
 * common bugs that occur with manual read() implementations.
 *
 * The driver gathers statistics by querying each subsystem through
 * the main device structure (struct echo_device).
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


/* ────────────────────────────────────────────────────────────────────
 * Private context
 * ────────────────────────────────────────────────────────────────────
 *
 * Each /proc entry stores a small context structure so we can keep
 * track of:
 *
 *   entry → pointer to the created /proc file
 *   dev   → pointer to the main Echo device structure
 *
 * The device pointer allows the proc code to query the current state
 * of all driver subsystems.
 */

struct echo_proc_ctx {
	struct proc_dir_entry *entry;  /* /proc entry created by this module */
	struct echo_device    *dev;    /* back-pointer to main driver state */
};


/* ────────────────────────────────────────────────────────────────────
 * seq_file show() callback
 * ────────────────────────────────────────────────────────────────────
 *
 * This function generates the text that appears when a user reads
 * /proc/echo_stats.
 *
 * The seq_file framework calls this function and provides a buffer
 * (struct seq_file *m) where formatted text should be written.
 *
 * m->private contains the pointer passed during open() (the device
 * pointer in our case).
 *
 * The function collects data from each subsystem and prints it using
 * seq_printf().
 */

static int echo_proc_show(struct seq_file *m, void *v)
{
	struct echo_device *dev = m->private;
	enum echo_mode cur_mode;
	const char *mode_str;
	u16 pan, tilt, tilt2;
	unsigned int buf_count;
	int total_moves, total_replays, irq_count, open_cnt;

	/* Get the current driver mode from the state subsystem */
	cur_mode = echo_state_get_mode(dev->state);

	/* Convert enum value to human-readable string */
	switch (cur_mode) {
	case ECHO_MODE_TEACH:  mode_str = "TEACH";  break;
	case ECHO_MODE_REPLAY: mode_str = "REPLAY"; break;
	default:               mode_str = "IDLE";   break;
	}

	/* Query current servo angles */
	pan   = echo_servo_get_angle(dev->servo, ECHO_SERVO_PAN);
	tilt  = echo_servo_get_angle(dev->servo, ECHO_SERVO_TILT);
	tilt2 = echo_servo_get_angle(dev->servo, ECHO_SERVO_TILT2);

	/* Retrieve statistics from other subsystems */
	buf_count     = echo_buffer_count(dev->buffer);
	total_moves   = echo_state_get_total_moves(dev->state);
	total_replays = echo_buffer_get_replay_count(dev->buffer);
	irq_count     = echo_joystick_get_irq_count(dev->joystick);
	open_cnt      = echo_chardev_get_open_count(dev->chardev);

	/* Print formatted statistics to the seq_file buffer */
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

	/* Additional driver configuration values */
	seq_printf(m, "Sim Mode:       %s\n", dev->sim_mode ? "yes" : "no");
	seq_printf(m, "Timeout:        %lu ms\n", dev->timeout_ms);

	return 0;
}


/* ────────────────────────────────────────────────────────────────────
 * proc file operations
 * ────────────────────────────────────────────────────────────────────
 *
 * procfs files use a struct proc_ops to define how the file behaves.
 *
 * Here we use the helper function single_open(), which simplifies the
 * seq_file setup for files that only need a single show() function.
 */

static int echo_proc_open(struct inode *inode, struct file *filp)
{
	/* pde_data() retrieves the pointer passed to proc_create_data() */
	return single_open(filp, echo_proc_show, pde_data(inode));
}

static const struct proc_ops echo_proc_ops = {
	.proc_open    = echo_proc_open,
	.proc_read    = seq_read,        /* seq_file read helper */
	.proc_lseek   = seq_lseek,       /* allow file seeking */
	.proc_release = single_release,  /* cleanup after read */
};


/* ────────────────────────────────────────────────────────────────────
 * Public API — create /proc entry
 * ────────────────────────────────────────────────────────────────────
 *
 * Called during driver initialization.
 *
 * Creates:
 *      /proc/echo_stats
 *
 * The proc entry stores the device pointer so the show() function
 * can access the driver's internal state.
 */

struct echo_proc_ctx *echo_proc_create(struct echo_device *dev)
{
	struct echo_proc_ctx *ctx;

	/* Allocate context structure */
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->dev = dev;

	/* Create the /proc entry */
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


/* ────────────────────────────────────────────────────────────────────
 * Public API — remove /proc entry
 * ────────────────────────────────────────────────────────────────────
 *
 * Called when the driver is unloaded.
 *
 * Removes the /proc entry and frees the context structure.
 */

void echo_proc_destroy(struct echo_proc_ctx *ctx)
{
	if (!ctx)
		return;

	remove_proc_entry("echo_stats", NULL);

	kfree(ctx);
}