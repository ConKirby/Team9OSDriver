/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_controller.c - Command handler for Project Echo.
 *
 * Processes single keypresses and translates them into write() or
 * ioctl() calls on the device fd.  A background thread is spawned
 * for REPLAY since write(REPLAY) blocks until playback finishes.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../module/echo_ioctl.h"
#include "echo_controller.h"

extern volatile sig_atomic_t running;

static char status_buf[128] = "Ready. Press a key to begin.";
static pthread_t replay_tid;
static volatile sig_atomic_t replaying;

const char *echo_controller_get_status(void)
{
	return status_buf;
}

/* Background thread for blocking REPLAY write */
static void *replay_worker(void *arg)
{
	int fd = (int)(long)arg;
	struct echo_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.command = ECHO_CMD_REPLAY;

	if (write(fd, &cmd, sizeof(cmd)) < 0)
		snprintf(status_buf, sizeof(status_buf),
			 "REPLAY failed: %s", strerror(errno));
	else
		snprintf(status_buf, sizeof(status_buf), "Replay complete.");

	replaying = 0;
	return NULL;
}

int echo_controller_handle_key(int fd, int ch)
{
	struct echo_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));

	switch (ch) {
	case '1':
	case 't':
	case 'T':
		cmd.command = ECHO_CMD_TEACH;
		if (write(fd, &cmd, sizeof(cmd)) < 0)
			snprintf(status_buf, sizeof(status_buf),
				 "TEACH failed: %s", strerror(errno));
		else
			snprintf(status_buf, sizeof(status_buf),
				 "Entered TEACH mode.");
		break;

	case '2':
	case 'r':
	case 'R':
		if (replaying) {
			snprintf(status_buf, sizeof(status_buf),
				 "Replay already in progress...");
			break;
		}
		replaying = 1;
		snprintf(status_buf, sizeof(status_buf), "Replaying...");
		pthread_create(&replay_tid, NULL, replay_worker,
			       (void *)(long)fd);
		pthread_detach(replay_tid);
		break;

	case '3':
	case 's':
	case 'S':
		cmd.command = ECHO_CMD_STOP;
		if (write(fd, &cmd, sizeof(cmd)) < 0)
			snprintf(status_buf, sizeof(status_buf),
				 "STOP failed: %s", strerror(errno));
		else
			snprintf(status_buf, sizeof(status_buf),
				 "Stopped.");
		break;

	case '6':
		if (ioctl(fd, ECHO_IOC_RESET) < 0)
			snprintf(status_buf, sizeof(status_buf),
				 "RESET failed: %s", strerror(errno));
		else
			snprintf(status_buf, sizeof(status_buf),
				 "Device reset.");
		break;

	case 'q':
	case 'Q':
		running = 0;
		return -1;

	default:
		/* Ignore unknown keys (including escape sequences) */
		break;
	}

	return 0;
}
