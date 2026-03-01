// SPDX-License-Identifier: GPL-2.0
/*
 * echo_chardev.c — Character device file_operations for Project Echo
 *
 * Implements open/release/read/write/ioctl for /dev/echo_robot.
 */

#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "echo_main.h"
#include "echo_ioctl.h"
#include "echo_chardev.h"
#include "echo_state.h"
#include "echo_buffer.h"
#include "echo_servo.h"

/* ------------------------------------------------------------------ */
/*  file_operations callbacks                                         */
/* ------------------------------------------------------------------ */

static int echo_open(struct inode *inode, struct file *filp)
{
	struct echo_device *dev = echo_dev;

	filp->private_data = dev;

	mutex_lock(&dev->open_lock);
	dev->open_count++;
	mutex_unlock(&dev->open_lock);

	pr_info("Device opened (count=%d)\n", dev->open_count);
	return 0;
}

static int echo_release(struct inode *inode, struct file *filp)
{
	struct echo_device *dev = filp->private_data;

	mutex_lock(&dev->open_lock);
	dev->open_count--;
	mutex_unlock(&dev->open_lock);

	pr_info("Device closed (count=%d)\n", dev->open_count);
	return 0;
}

static ssize_t echo_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct echo_device *dev = filp->private_data;
	struct echo_state_snapshot snap;
	unsigned long flags;
	size_t to_copy;

	/* Non-blocking check */
	if ((filp->f_flags & O_NONBLOCK) && !dev->new_data_avail)
		return -EAGAIN;

	/* Block until new data is available */
	if (wait_event_interruptible(dev->wq_read, dev->new_data_avail))
		return -ERESTARTSYS;

	dev->new_data_avail = false;

	/* Build the snapshot under the appropriate locks */
	spin_lock_irqsave(&dev->mode_lock, flags);
	snap.mode = (u32)dev->mode;
	spin_unlock_irqrestore(&dev->mode_lock, flags);

	mutex_lock(&dev->servo_lock);
	snap.pan_angle  = dev->servo_pos[ECHO_SERVO_PAN];
	snap.tilt_angle = dev->servo_pos[ECHO_SERVO_TILT];
	mutex_unlock(&dev->servo_lock);

	snap.buffer_count  = echo_buffer_count(dev);
	snap.total_moves   = (u32)atomic_read(&dev->stat_total_moves);
	snap.total_replays = (u32)atomic_read(&dev->stat_replays);

	to_copy = min(count, sizeof(snap));
	if (copy_to_user(buf, &snap, to_copy))
		return -EFAULT;

	return (ssize_t)to_copy;
}

static ssize_t echo_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct echo_device *dev = filp->private_data;
	struct echo_cmd cmd;
	int ret;

	if (count < sizeof(struct echo_cmd))
		return -EINVAL;

	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	switch (cmd.command) {
	case ECHO_CMD_TEACH:
		echo_state_set_mode(dev, ECHO_MODE_TEACH);
		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
		break;

	case ECHO_CMD_REPLAY:
		dev->replay_finished = false;
		ret = echo_state_start_replay(dev);
		if (ret)
			return ret;

		/* Blocking write — wait for replay to finish */
		if (wait_event_interruptible(dev->wq_replay_done,
					     dev->replay_finished))
			return -ERESTARTSYS;
		break;

	case ECHO_CMD_STOP:
		echo_state_stop(dev);
		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
		break;

	case ECHO_CMD_MOVE:
		ret = echo_servo_set_angle(dev, (u8)cmd.servo_id,
					   (u16)cmd.angle);
		if (ret)
			return ret;
		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
		break;

	default:
		return -EINVAL;
	}

	return (ssize_t)count;
}

static long echo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct echo_device *dev = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case ECHO_IOC_SET_SPEED: {
		u16 speed;

		if (copy_from_user(&speed, uarg, sizeof(speed)))
			return -EFAULT;
		if (speed == 0)
			return -EINVAL;
		dev->replay_speed = speed;
		return 0;
	}

	case ECHO_IOC_RESET:
		echo_state_stop(dev);
		echo_buffer_clear(dev);
		echo_servo_set_angle(dev, ECHO_SERVO_PAN, ECHO_SERVO_CENTER);
		echo_servo_set_angle(dev, ECHO_SERVO_TILT, ECHO_SERVO_CENTER);
		dev->new_data_avail = true;
		wake_up_interruptible(&dev->wq_read);
		return 0;

	case ECHO_IOC_GET_STATE: {
		struct echo_ioctl_state st;
		unsigned long flags;

		spin_lock_irqsave(&dev->mode_lock, flags);
		st.mode = (u32)dev->mode;
		spin_unlock_irqrestore(&dev->mode_lock, flags);

		mutex_lock(&dev->servo_lock);
		st.pan_angle  = dev->servo_pos[ECHO_SERVO_PAN];
		st.tilt_angle = dev->servo_pos[ECHO_SERVO_TILT];
		mutex_unlock(&dev->servo_lock);

		st.buffer_count  = echo_buffer_count(dev);
		st.total_moves   = (u32)atomic_read(&dev->stat_total_moves);
		st.total_replays = (u32)atomic_read(&dev->stat_replays);
		st.irq_count     = (u32)atomic_read(&dev->stat_irq_count);

		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}

	case ECHO_IOC_SET_MODE: {
		u32 new_mode;

		if (copy_from_user(&new_mode, uarg, sizeof(new_mode)))
			return -EFAULT;
		if (new_mode > ECHO_MODE_REPLAY)
			return -EINVAL;
		return echo_state_set_mode(dev, (enum echo_mode)new_mode);
	}

	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/*  file_operations structure                                         */
/* ------------------------------------------------------------------ */

static const struct file_operations echo_fops = {
	.owner          = THIS_MODULE,
	.open           = echo_open,
	.release        = echo_release,
	.read           = echo_read,
	.write          = echo_write,
	.unlocked_ioctl = echo_ioctl,
};

/* ------------------------------------------------------------------ */
/*  chardev init / cleanup                                            */
/* ------------------------------------------------------------------ */

int echo_chardev_init(struct echo_device *dev)
{
	int ret;

	/* 1. Allocate a dynamic major + minor */
	ret = alloc_chrdev_region(&dev->devno, 0, 1, ECHO_DEVICE_NAME);
	if (ret < 0) {
		pr_err("echo: alloc_chrdev_region failed (%d)\n", ret);
		goto fail_region;
	}

	/* 2. Initialise and add the cdev */
	cdev_init(&dev->cdev, &echo_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev->devno, 1);
	if (ret < 0) {
		pr_err("echo: cdev_add failed (%d)\n", ret);
		goto fail_cdev;
	}

	/* 3. Create device class (single-arg form for kernel >= 6.x) */
	dev->dev_class = class_create(ECHO_CLASS_NAME);
	if (IS_ERR(dev->dev_class)) {
		ret = PTR_ERR(dev->dev_class);
		pr_err("echo: class_create failed (%d)\n", ret);
		goto fail_class;
	}

	/* 4. Create the device node under /dev */
	dev->device = device_create(dev->dev_class, NULL, dev->devno,
				    NULL, ECHO_DEVICE_NAME);
	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		pr_err("echo: device_create failed (%d)\n", ret);
		goto fail_device;
	}

	pr_info("echo: chardev registered (%d:%d)\n",
		MAJOR(dev->devno), MINOR(dev->devno));
	return 0;

fail_device:
	class_destroy(dev->dev_class);
fail_class:
	cdev_del(&dev->cdev);
fail_cdev:
	unregister_chrdev_region(dev->devno, 1);
fail_region:
	return ret;
}

void echo_chardev_cleanup(struct echo_device *dev)
{
	if (dev->device)
		device_destroy(dev->dev_class, dev->devno);
	if (dev->dev_class)
		class_destroy(dev->dev_class);
	cdev_del(&dev->cdev);
	unregister_chrdev_region(dev->devno, 1);
}
