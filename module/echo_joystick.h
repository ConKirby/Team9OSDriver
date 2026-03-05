/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_joystick.h — GPIO joystick input subsystem
 *
 * Uses echo_joystick_ops callbacks so the joystick never directly
 * calls into the state machine or any other subsystem.
 */
#ifndef ECHO_JOYSTICK_H
#define ECHO_JOYSTICK_H

#include <linux/types.h>

#include "echo_types.h"  /* ECHO_NUM_GPIO */

struct echo_joystick_ctx;

/* Callbacks the joystick fires on physical input */
struct echo_joystick_ops {
	void (*on_direction)(void *data, u8 servo_id, int delta);
	void (*on_button)(void *data);
};

struct echo_joystick_ctx *echo_joystick_create(
		const int gpio_pins[ECHO_NUM_GPIO],
		bool sim_mode,
		const struct echo_joystick_ops *ops,
		void *ops_data);
void echo_joystick_destroy(struct echo_joystick_ctx *ctx);

int echo_joystick_get_irq_count(struct echo_joystick_ctx *ctx);

#endif /* ECHO_JOYSTICK_H */
