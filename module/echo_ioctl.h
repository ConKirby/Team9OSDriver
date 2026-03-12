/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_ioctl.h — Shared header between kernel module and user-space app
 *
 * Defines the ioctl numbers, command struct for write(), and the
 * unified device snapshot struct used by both read() and ioctl.
 */
#ifndef ECHO_IOCTL_H
#define ECHO_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#endif

/* ── Mode values (matches enum echo_mode in kernel) ────────────────── */
#define ECHO_MODE_VAL_IDLE	0
#define ECHO_MODE_VAL_TEACH	1
#define ECHO_MODE_VAL_REPLAY	2

/* ── Write command IDs ─────────────────────────────────────────────── */
#define ECHO_CMD_TEACH		1
#define ECHO_CMD_REPLAY		2
#define ECHO_CMD_STOP		3
#define ECHO_CMD_MOVE		4

/* ── Write command structure (user -> kernel via write()) ──────────── */
struct echo_cmd {
	__u32 command;
	__u32 servo_id;
	__u32 angle;
	__u32 speed;
};

/* ── Device snapshot (kernel -> user via read() and ioctl) ─────────── */
struct echo_snapshot {
	__u32 mode;
	__u16 pan_angle;
	__u16 tilt_angle;
	__u16 tilt2_angle;
	__u32 buffer_count;
	__u32 total_moves;
	__u32 total_replays;
	__u32 irq_count;
};

/* ── Ioctl numbers ─────────────────────────────────────────────────── */
#define ECHO_IOC_MAGIC		'E'
#define ECHO_IOC_SET_SPEED	_IOW(ECHO_IOC_MAGIC, 1, __u16)
#define ECHO_IOC_RESET		_IO(ECHO_IOC_MAGIC, 2)
#define ECHO_IOC_GET_STATE	_IOR(ECHO_IOC_MAGIC, 3, struct echo_snapshot)
#define ECHO_IOC_SET_MODE	_IOW(ECHO_IOC_MAGIC, 4, __u32)

#endif /* ECHO_IOCTL_H */
