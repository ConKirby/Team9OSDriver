/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_ioctl.h — Shared interface between kernel module and user-space
 *
 * This header defines the binary API used by the user-space controller
 * application to communicate with the Echo robot kernel driver.
 *
 * Both the kernel module and the user program must include this file
 * so that they agree on:
 *
 *   • ioctl command numbers
 *   • command packet formats for write()
 *   • snapshot data returned by read() or ioctl
 *
 * IMPORTANT:
 * This file forms part of the *ABI* between user-space and kernel-space.
 * Any changes must remain backward-compatible or the user program will
 * break when communicating with the driver.
 */

#ifndef ECHO_IOCTL_H
#define ECHO_IOCTL_H

/*
 * When compiled inside the kernel module, we must use kernel headers.
 * When compiled in user-space, we use the user-space equivalents.
 *
 * __KERNEL__ is defined automatically by the kernel build system.
 */
#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#endif


/* ────────────────────────────────────────────────────────────────────
 * Mode values
 * ────────────────────────────────────────────────────────────────────
 *
 * These numeric values mirror the kernel's internal enum echo_mode.
 * They are duplicated here so that user-space does not need access
 * to kernel-only headers such as echo_types.h.
 *
 * The user application may send these values via ioctl() or receive
 * them when reading device state.
 */

#define ECHO_MODE_VAL_IDLE	0   /* Robot is idle (not recording or replaying) */
#define ECHO_MODE_VAL_TEACH	1   /* Teach mode: movements are recorded */
#define ECHO_MODE_VAL_REPLAY	2   /* Replay mode: recorded movements are executed */


/* ────────────────────────────────────────────────────────────────────
 * Write command IDs
 * ────────────────────────────────────────────────────────────────────
 *
 * Commands sent from user-space to the driver via write().
 *
 * The user application writes a struct echo_cmd to the device file
 * (e.g. /dev/echo_robot). The kernel reads the structure and performs
 * the requested action.
 */

#define ECHO_CMD_TEACH		1   /* Start recording joystick movements */
#define ECHO_CMD_REPLAY		2   /* Replay the recorded movement sequence */
#define ECHO_CMD_STOP		3   /* Stop any ongoing teach/replay activity */
#define ECHO_CMD_MOVE		4   /* Direct servo movement command */


/* ────────────────────────────────────────────────────────────────────
 * Write command structure
 * ────────────────────────────────────────────────────────────────────
 *
 * This structure is written by user-space using write().
 *
 * Example flow:
 *   user app -> write(struct echo_cmd) -> kernel driver
 *
 * Fields:
 *   command  : one of the ECHO_CMD_* values above
 *   servo_id : which servo to control (pan / tilt / tilt2)
 *   angle    : target servo angle (0–180 degrees)
 *   speed    : optional speed parameter (driver-specific meaning)
 */

struct echo_cmd {
	__u32 command;     /* Command ID (ECHO_CMD_*) */
	__u32 servo_id;    /* Servo index to control */
	__u32 angle;       /* Target angle in degrees */
	__u32 speed;       /* Movement speed parameter */
};


/* ────────────────────────────────────────────────────────────────────
 * Device snapshot
 * ────────────────────────────────────────────────────────────────────
 *
 * This structure represents a snapshot of the robot's current state.
 *
 * It can be retrieved in two ways:
 *   • read() on the device file
 *   • ioctl(ECHO_IOC_GET_STATE)
 *
 * The kernel fills this structure and copies it to user-space.
 */

struct echo_snapshot {
	__u32 mode;          /* Current operating mode (ECHO_MODE_VAL_*) */

	/* Current servo angles */
	__u16 pan_angle;
	__u16 tilt_angle;
	__u16 tilt2_angle;

	/* Statistics and runtime counters */
	__u32 buffer_count;  /* Number of recorded movements in buffer */
	__u32 total_moves;   /* Total moves recorded since module load */
	__u32 total_replays; /* Number of replay operations performed */
	__u32 irq_count;     /* Total joystick interrupts handled */
};


/* ────────────────────────────────────────────────────────────────────
 * Ioctl definitions
 * ────────────────────────────────────────────────────────────────────
 *
 * ioctl commands allow structured control operations that are not
 * suitable for simple read()/write().
 *
 * The _IO*, _IOR, _IOW macros encode:
 *
 *   magic number  → identifies this driver
 *   command id    → operation number
 *   data type     → structure passed between user and kernel
 *
 * Magic numbers should be unique per driver to avoid collisions.
 */

#define ECHO_IOC_MAGIC		'E'

/*
 * Set global servo movement speed.
 * Argument: __u16 speed value from user-space.
 */
#define ECHO_IOC_SET_SPEED	_IOW(ECHO_IOC_MAGIC, 1, __u16)

/*
 * Reset the driver state.
 * Clears buffers, counters, and returns the robot to idle mode.
 */
#define ECHO_IOC_RESET		_IO(ECHO_IOC_MAGIC, 2)

/*
 * Retrieve the current device state.
 * User provides struct echo_snapshot which kernel fills.
 */
#define ECHO_IOC_GET_STATE	_IOR(ECHO_IOC_MAGIC, 3, struct echo_snapshot)

/*
 * Force the device into a specific operating mode.
 * Argument must be one of the ECHO_MODE_VAL_* values.
 */
#define ECHO_IOC_SET_MODE	_IOW(ECHO_IOC_MAGIC, 4, __u32)

#endif /* ECHO_IOCTL_H */