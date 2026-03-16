/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_types.h — Shared constants, enums, and kernel-only types
 *
 * Included by every subsystem .c file for common definitions.
 * Does NOT contain struct echo_device (see echo_main.h).
 */
#ifndef ECHO_TYPES_H
#define ECHO_TYPES_H

#include <linux/types.h>

/* ── Device identity ───────────────────────────────────────────────── */
#define ECHO_DEVICE_NAME	"echo_robot"
#define ECHO_CLASS_NAME		"echo"

/* ── Sizes and limits ──────────────────────────────────────────────── */
#define ECHO_FIFO_SIZE		256	/* must be power of 2 for kfifo */

/* ── Servo constants ───────────────────────────────────────────────── */
#define ECHO_NUM_SERVOS		3
#define ECHO_SERVO_PAN		0	/* base rotation — channel 0 */
#define ECHO_SERVO_TILT		1	/* first arm     — channel 1 */
#define ECHO_SERVO_TILT2	2	/* second arm    — channel 2 */
#define ECHO_SERVO_CENTER	90
#define ECHO_SERVO_MIN		0
#define ECHO_SERVO_MAX		180

/* ── GPIO / joystick constants ─────────────────────────────────────── */
#define ECHO_NUM_GPIO		4
#define ECHO_GPIO_UP		0
#define ECHO_GPIO_DOWN		1
#define ECHO_GPIO_LEFT		2
#define ECHO_GPIO_RIGHT		3

/* ── Timing and step constants ─────────────────────────────────────── */
#define ECHO_DEFAULT_TIMEOUT_MS		5000
#define ECHO_DEBOUNCE_MS		50
#define ECHO_REPLAY_STEP_MS		50
#define ECHO_ANGLE_STEP			5

/* ── Operating modes ───────────────────────────────────────────────── */
enum echo_mode {
	ECHO_MODE_IDLE   = 0,
	ECHO_MODE_TEACH  = 1,
	ECHO_MODE_REPLAY = 2,
};

/* ── A single recorded servo movement ──────────────────────────────── */
struct echo_move {
	u8  servo_id;	/* ECHO_SERVO_PAN, ECHO_SERVO_TILT, or ECHO_SERVO_TILT2 */
	u16 angle;	/* 0–180 degrees */
	u32 delay_ms;	/* time since previous move */
};

#endif /* ECHO_TYPES_H */