/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_IOCTL_H
#define ECHO_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#endif

#define ECHO_IOC_MAGIC  'E'

/* Ioctl: set replay speed multiplier (arg: __u16, 1=normal, 2=double, etc.) */
#define ECHO_IOC_SET_SPEED   _IOW(ECHO_IOC_MAGIC, 1, __u16)

/* Ioctl: reset device — stop, clear buffer, center servos */
#define ECHO_IOC_RESET       _IO(ECHO_IOC_MAGIC, 2)

/* Ioctl: get current state */
struct echo_ioctl_state {
	__u32 mode;          /* enum echo_mode */
	__u16 pan_angle;
	__u16 tilt_angle;
	__u32 buffer_count;
	__u32 total_moves;
	__u32 total_replays;
	__u32 irq_count;
};
#define ECHO_IOC_GET_STATE   _IOR(ECHO_IOC_MAGIC, 3, struct echo_ioctl_state)

/* Ioctl: force mode transition (arg: __u32, one of ECHO_MODE_*) */
#define ECHO_IOC_SET_MODE    _IOW(ECHO_IOC_MAGIC, 4, __u32)

#endif /* ECHO_IOCTL_H */
