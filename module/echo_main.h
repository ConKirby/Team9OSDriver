/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_main.h — Master device structure (the coordinator)
 *
 * Only included by echo_main.c, echo_chardev.c, and echo_proc.c —
 * the files that need to see the full echo_device layout.
 * Subsystems (servo, buffer, state, joystick) do NOT include this;
 * they receive opaque void* pointers via their ops callbacks.
 */
#ifndef ECHO_MAIN_H
#define ECHO_MAIN_H

#include <linux/wait.h>

#include "echo_types.h"

/* ── Forward declarations — subsystem contexts are opaque ──────────── */
struct echo_servo_ctx;
struct echo_buffer_ctx;
struct echo_state_ctx;
struct echo_joystick_ctx;
struct echo_chardev_ctx;
struct echo_proc_ctx;

/*
 * struct echo_device — the coordinator
 *
 * Each subsystem owns its private state inside an opaque *_ctx.
 * This struct holds pointers to those contexts plus the shared
 * blocking-I/O primitives that span multiple subsystems.
 *
 * Dependency graph (all wired in echo_main.c):
 *
 *   joystick --[ops]--> main ---> state
 *   state    --[ops]--> main ---> servo, buffer
 *   buffer   --[ops]--> main ---> servo, state (mode check)
 *   chardev  ---------> echo_device ---> all subsystem APIs
 *   proc     ---------> echo_device ---> all subsystem APIs (read-only)
 */
struct echo_device {
	/* Subsystem contexts (opaque — never dereferenced here) */
	struct echo_servo_ctx    *servo;
	struct echo_buffer_ctx   *buffer;
	struct echo_state_ctx    *state;
	struct echo_joystick_ctx *joystick;
	struct echo_chardev_ctx  *chardev;
	struct echo_proc_ctx     *proc;

	/* Blocking I/O coordination (shared across chardev, state, buffer) */
	wait_queue_head_t wq_read;
	wait_queue_head_t wq_replay_done;
	bool new_data_avail;
	bool replay_finished;

	/* Configuration (set once at init, read-only afterwards) */
	bool sim_mode;
	unsigned long timeout_ms;
};

#endif /* ECHO_MAIN_H */
