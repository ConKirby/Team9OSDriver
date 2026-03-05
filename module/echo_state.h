/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_state.h — State machine + inactivity timer subsystem
 *
 * Uses echo_state_ops callbacks for all cross-module actions
 * (servo control, buffer recording, replay triggering).
 */
#ifndef ECHO_STATE_H
#define ECHO_STATE_H

#include <linux/types.h>

#include "echo_types.h"  /* enum echo_mode */

struct echo_state_ctx;
struct echo_move;

/* Callbacks the state machine invokes for cross-module actions */
struct echo_state_ops {
	int  (*move_servo)(void *data, u8 servo_id, u16 angle);
	u16  (*get_servo)(void *data, u8 servo_id);
	void (*record_move)(void *data, const struct echo_move *move);
	void (*clear_buffer)(void *data);
	void (*start_replay)(void *data);
	void (*cancel_replay)(void *data);
	void (*notify)(void *data);
};

struct echo_state_ctx *echo_state_create(unsigned long timeout_ms,
					 const struct echo_state_ops *ops,
					 void *ops_data);
void echo_state_destroy(struct echo_state_ctx *ctx);

void echo_state_handle_input(struct echo_state_ctx *ctx,
			     u8 servo_id, int delta);
int  echo_state_start_replay(struct echo_state_ctx *ctx);
void echo_state_stop(struct echo_state_ctx *ctx);
int  echo_state_set_mode(struct echo_state_ctx *ctx, enum echo_mode new_mode);
enum echo_mode echo_state_get_mode(struct echo_state_ctx *ctx);

/* Called by the replay worker when replay finishes naturally */
void echo_state_replay_complete(struct echo_state_ctx *ctx);

int echo_state_get_total_moves(struct echo_state_ctx *ctx);

#endif /* ECHO_STATE_H */
