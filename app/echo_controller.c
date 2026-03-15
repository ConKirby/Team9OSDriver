// echo_controller.c - Command handler for Project Echo.
// Processes single keypresses and translates them into write() or
// ioctl() calls on the device fd.  A background thread is spawned
// for REPLAY since write(REPLAY) blocks until playback finishes.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Header file imports
// icotl is the join between the kernel and user space
#include "../module/echo_ioctl.h"
#include "echo_controller.h"

// Running var defined in echo_app.c
extern volatile sig_atomic_t running;

// shared between controller and visualiser
static char status_buf[128] = "Ready. Press a key to begin.";
static pthread_t replay_tid;
// Guard flag to protect against multiple REPLAYs
static volatile sig_atomic_t replaying;

// called by echo_app.c on every render cycle
const char *echo_controller_get_status(void)
{
	return status_buf;
}

// Background thread for blocking REPLAY write
static void *replay_worker(void *arg)
{
	// reverse cast of (void *)(long)fd
	int fd = (int)(long)arg;

	struct echo_cmd cmd;

	// Zero the struct so `servo_id`, `angle`, `speed` are all 0
	memset(&cmd, 0, sizeof(cmd));
	// Set the command to REPLAY
	cmd.command = ECHO_CMD_REPLAY;

	 // write() blocks in kernel until replay finishes — that's why this runs
    // in a separate thread (the main thread would freeze otherwise).
	if (write(fd, &cmd, sizeof(cmd)) < 0)
		snprintf(status_buf, sizeof(status_buf),
			 "REPLAY failed: %s", strerror(errno));
	else
		snprintf(status_buf, sizeof(status_buf), "Replay complete.");

	 // clear guard so user can start another replay      
	replaying = 0;
	return NULL;
}

int echo_controller_handle_key(int fd, int ch)
{
	struct echo_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));

	switch (ch) {
		// Teach mode
	case '1':
	case 't':
	case 'T':
		cmd.command = ECHO_CMD_TEACH;
		// send an echo to the kernel
		// Teach does not block
		if (write(fd, &cmd, sizeof(cmd)) < 0)
			snprintf(status_buf, sizeof(status_buf), "TEACH failed: %s", strerror(errno));
		else
			snprintf(status_buf, sizeof(status_buf), "Entered TEACH mode.");
		break;

	// Reply mode
	case '2':
	case 'r':
	case 'R':
	// prevents spawning a second replay
		if (replaying) {
			snprintf(status_buf, sizeof(status_buf), "Replay already in progress...");
			break;
		}
		replaying = 1;
		snprintf(status_buf, sizeof(status_buf), "Replaying...");
		// spawn replay worker in new thread
		pthread_create(&replay_tid, NULL, replay_worker, (void *)(long)fd);
		// we want main thread to wait until repaly finishes
		// join would risk zombie thread
		pthread_detach(replay_tid);
		break;

	// Stop
	case '3':
	case 's':
	case 'S':
	// set mode to idle
		cmd.command = ECHO_CMD_STOP;
		if (write(fd, &cmd, sizeof(cmd)) < 0){
			snprintf(status_buf, sizeof(status_buf), "STOP failed: %s", strerror(errno));
		}
		else{
			snprintf(status_buf, sizeof(status_buf),"Stopped.");
		}
		break;

	case '6':
	// 6 is reset
	// Stops the state machine, Clears recording buffer, Centers all three servos to 90 degrees and Notifies readers
		if (ioctl(fd, ECHO_IOC_RESET) < 0){
			snprintf(status_buf, sizeof(status_buf), "RESET failed: %s", strerror(errno));
		}
		else{
			snprintf(status_buf, sizeof(status_buf), "Device reset.");
		}
		break;

	case 'q':
	case 'Q':
	// set to not running
		running = 0;
		return -1;

	default:
		// Ignore unknown keys (including escape sequences)
		break;
	}

	return 0;
}
