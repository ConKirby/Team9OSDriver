/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_buffer.h — kfifo ring buffer + replay worker subsystem
 *
 * Uses echo_buffer_ops callbacks for all cross-module actions
 * (servo moves, stop checks, completion signalling).
 */
#ifndef ECHO_BUFFER_H
#define ECHO_BUFFER_H

#include <linux/types.h>

struct echo_buffer_ctx;
struct echo_move;

/* Callbacks the buffer subsystem invokes during replay */
struct echo_buffer_ops {
	int  (*move_servo)(void *data, u8 servo_id, u16 angle);
	bool (*should_stop)(void *data);
	void (*replay_done)(void *data);
	void (*notify)(void *data);
};

struct echo_buffer_ctx *echo_buffer_create(const struct echo_buffer_ops *ops,
					   void *ops_data);
void echo_buffer_destroy(struct echo_buffer_ctx *ctx);

int  echo_buffer_record(struct echo_buffer_ctx *ctx, const struct echo_move *move);
void echo_buffer_clear(struct echo_buffer_ctx *ctx);
unsigned int echo_buffer_count(struct echo_buffer_ctx *ctx);

void echo_buffer_start_replay(struct echo_buffer_ctx *ctx);
void echo_buffer_cancel_replay(struct echo_buffer_ctx *ctx);
void echo_buffer_set_replay_speed(struct echo_buffer_ctx *ctx, u16 speed);
int  echo_buffer_get_replay_count(struct echo_buffer_ctx *ctx);

#endif /* ECHO_BUFFER_H */
