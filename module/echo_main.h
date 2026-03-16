//echo_main.h — Master device structure
#ifndef ECHO_MAIN_H
#define ECHO_MAIN_H

#include <linux/wait.h>

#include "echo_types.h"

//forward declarations— subsystem contexts are opaque
struct echo_servo_ctx;
struct echo_buffer_ctx;
struct echo_state_ctx;
struct echo_joystick_ctx;
struct echo_chardev_ctx;
struct echo_proc_ctx;

// struct echo_device — the coordinator
struct echo_device {
	// Subsystem contexts (opaque — never dereferenced here)
	struct echo_servo_ctx    *servo;
	struct echo_buffer_ctx   *buffer;
	struct echo_state_ctx    *state;
	struct echo_joystick_ctx *joystick;
	struct echo_chardev_ctx  *chardev;
	struct echo_proc_ctx     *proc;

	// Synchronization
	wait_queue_head_t wq_read;
	wait_queue_head_t wq_replay_done;
	bool new_data_avail;
	bool replay_finished;

	//configuration (set once at init, read only afterwards)
	bool sim_mode;
	unsigned long timeout_ms;
};

#endif 
