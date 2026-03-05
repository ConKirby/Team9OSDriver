// SPDX-License-Identifier: GPL-2.0
/*
 * echo_chardev.c — Character device file_operations for /dev/echo_robot
 *
 * The chardev is the user-space gateway.  It holds a back-pointer to
 * echo_device and calls all subsystem APIs through it.
 *
 * Dependencies: echo_device (echo_main.h) → all subsystem public APIs.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "echo_main.h"
#include "echo_ioctl.h"
#include "echo_chardev.h"
#include "echo_servo.h"
#include "echo_state.h"
#include "echo_buffer.h"
#include "echo_joystick.h"

/* ── Private context ───────────────────────────────────────────────── */
struct echo_chardev_ctx {
	struct cdev    cdev;
	dev_t          devno;
	struct class  *dev_class;
	struct device *device;

	int            open_count;
	struct mutex   open_lock;

	struct echo_device *dev;	/* back-pointer to coordinator */
};

/* ── Helper: build a snapshot from all subsystems ──────────────────── */

static void build_snapshot(struct echo_device *dev, struct echo_snapshot *snap)
{
	snap->mode          = (u32)echo_state_get_mode(dev->state);
	snap->pan_angle     = echo_servo_get_angle(dev->servo, ECHO_SERVO_PAN);
	snap->tilt_angle    = echo_servo_get_angle(dev->servo, ECHO_SERVO_TILT);
	snap->buffer_count  = echo_buffer_count(dev->buffer);
	snap->total_moves   = (u32)echo_state_get_total_moves(dev->state);
	snap->total_replays = (u32)echo_buffer_get_replay_count(dev->buffer);
	snap->irq_count     = (u32)echo_joystick_get_irq_count(dev->joystick);
}

/* ── Helper: set new_data_avail + wake readers ─────────────────────── */

static void notify_readers(struct echo_device *dev)
{
	WRITE_ONCE(dev->new_data_avail, true);
	wake_up_interruptible(&dev->wq_read);
}

/* ── file_operations callbacks ─────────────────────────────────────── */

static int echo_open(struct inode *inode, struct file *filp)
{
	struct echo_chardev_ctx *ctx =
		container_of(inode->i_cdev, struct echo_chardev_ctx, cdev);

	filp->private_data = ctx;

	mutex_lock(&ctx->open_lock);
	ctx->open_count++;
	mutex_unlock(&ctx->open_lock);

	pr_info("echo: chardev: opened (count=%d)\n", ctx->open_count);
	return 0;
}

static int echo_release(struct inode *inode, struct file *filp)
{
	struct echo_chardev_ctx *ctx = filp->private_data;

	mutex_lock(&ctx->open_lock);
	ctx->open_count--;
	mutex_unlock(&ctx->open_lock);

	pr_info("echo: chardev: closed (count=%d)\n", ctx->open_count);
	return 0;
}

static ssize_t echo_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct echo_chardev_ctx *ctx = filp->private_data;
	struct echo_device *dev = ctx->dev;
	struct echo_snapshot snap;
	size_t to_copy;

	/* Non-blocking check */
	if ((filp->f_flags & O_NONBLOCK) && !READ_ONCE(dev->new_data_avail))
		return -EAGAIN;

	/* Block until new data is available */
	if (wait_event_interruptible(dev->wq_read,
				     READ_ONCE(dev->new_data_avail)))
		return -ERESTARTSYS;

	WRITE_ONCE(dev->new_data_avail, false);

	build_snapshot(dev, &snap);

	to_copy = min(count, sizeof(snap));
	if (copy_to_user(buf, &snap, to_copy))
		return -EFAULT;

	return (ssize_t)to_copy;
}

static ssize_t echo_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct echo_chardev_ctx *ctx = filp->private_data;
	struct echo_device *dev = ctx->dev;
	struct echo_cmd cmd;
	int ret;

	if (count < sizeof(struct echo_cmd))
		return -EINVAL;

	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	switch (cmd.command) {
	case ECHO_CMD_TEACH:
		echo_state_set_mode(dev->state, ECHO_MODE_TEACH);
		break;

	case ECHO_CMD_REPLAY:
		WRITE_ONCE(dev->replay_finished, false);
		ret = echo_state_start_replay(dev->state);
		if (ret)
			return ret;
		/* Blocking write — sleep until replay completes */
		if (wait_event_interruptible(dev->wq_replay_done,
					     READ_ONCE(dev->replay_finished)))
			return -ERESTARTSYS;
		break;

	case ECHO_CMD_STOP:
		echo_state_stop(dev->state);
		break;

	case ECHO_CMD_MOVE:
		ret = echo_servo_set_angle(dev->servo,
					   (u8)cmd.servo_id,
					   (u16)cmd.angle);
		if (ret)
			return ret;
		notify_readers(dev);
		break;

	default:
		return -EINVAL;
	}

	return (ssize_t)count;
}

static long echo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct echo_chardev_ctx *ctx = filp->private_data;
	struct echo_device *dev = ctx->dev;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case ECHO_IOC_SET_SPEED: {
		u16 speed;

		if (copy_from_user(&speed, uarg, sizeof(speed)))
			return -EFAULT;
		if (speed == 0)
			return -EINVAL;
		echo_buffer_set_replay_speed(dev->buffer, speed);
		return 0;
	}

	case ECHO_IOC_RESET:
		echo_state_stop(dev->state);
		echo_buffer_clear(dev->buffer);
		echo_servo_set_angle(dev->servo, ECHO_SERVO_PAN,
				     ECHO_SERVO_CENTER);
		echo_servo_set_angle(dev->servo, ECHO_SERVO_TILT,
				     ECHO_SERVO_CENTER);
		notify_readers(dev);
		return 0;

	case ECHO_IOC_GET_STATE: {
		struct echo_snapshot snap;

		build_snapshot(dev, &snap);
		if (copy_to_user(uarg, &snap, sizeof(snap)))
			return -EFAULT;
		return 0;
	}

	case ECHO_IOC_SET_MODE: {
		u32 new_mode;

		if (copy_from_user(&new_mode, uarg, sizeof(new_mode)))
			return -EFAULT;
		if (new_mode > ECHO_MODE_REPLAY)
			return -EINVAL;
		return echo_state_set_mode(dev->state,
					   (enum echo_mode)new_mode);
	}

	default:
		return -ENOTTY;
	}
}

/* ── file_operations table ─────────────────────────────────────────── */

static const struct file_operations echo_fops = {
	.owner          = THIS_MODULE,
	.open           = echo_open,
	.release        = echo_release,
	.read           = echo_read,
	.write          = echo_write,
	.unlocked_ioctl = echo_ioctl,
};

/* ── Public API ────────────────────────────────────────────────────── */

struct echo_chardev_ctx *echo_chardev_create(struct echo_device *dev)
{
	struct echo_chardev_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	mutex_init(&ctx->open_lock);
	ctx->dev = dev;

	/* 1. Allocate a dynamic major + minor */
	ret = alloc_chrdev_region(&ctx->devno, 0, 1, ECHO_DEVICE_NAME);
	if (ret < 0) {
		pr_err("echo: chardev: alloc_chrdev_region failed (%d)\n", ret);
		goto fail_alloc;
	}

	/* 2. Initialise and add the cdev */
	cdev_init(&ctx->cdev, &echo_fops);
	ctx->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ctx->cdev, ctx->devno, 1);
	if (ret < 0) {
		pr_err("echo: chardev: cdev_add failed (%d)\n", ret);
		goto fail_cdev;
	}

	/* 3. Create device class */
	ctx->dev_class = class_create(ECHO_CLASS_NAME);
	if (IS_ERR(ctx->dev_class)) {
		ret = PTR_ERR(ctx->dev_class);
		pr_err("echo: chardev: class_create failed (%d)\n", ret);
		goto fail_class;
	}

	/* 4. Create the /dev node */
	ctx->device = device_create(ctx->dev_class, NULL, ctx->devno,
				    NULL, ECHO_DEVICE_NAME);
	if (IS_ERR(ctx->device)) {
		ret = PTR_ERR(ctx->device);
		pr_err("echo: chardev: device_create failed (%d)\n", ret);
		goto fail_device;
	}

	pr_info("echo: chardev: registered (%d:%d)\n",
		MAJOR(ctx->devno), MINOR(ctx->devno));
	return ctx;

fail_device:
	class_destroy(ctx->dev_class);
fail_class:
	cdev_del(&ctx->cdev);
fail_cdev:
	unregister_chrdev_region(ctx->devno, 1);
fail_alloc:
	kfree(ctx);
	return ERR_PTR(ret);
}

void echo_chardev_destroy(struct echo_chardev_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->device)
		device_destroy(ctx->dev_class, ctx->devno);
	if (ctx->dev_class)
		class_destroy(ctx->dev_class);
	cdev_del(&ctx->cdev);
	unregister_chrdev_region(ctx->devno, 1);
	kfree(ctx);
}

int echo_chardev_get_open_count(struct echo_chardev_ctx *ctx)
{
	int count;

	mutex_lock(&ctx->open_lock);
	count = ctx->open_count;
	mutex_unlock(&ctx->open_lock);

	return count;
}
