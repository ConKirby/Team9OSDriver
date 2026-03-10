/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_app.c - Main entry point for the Project Echo user-space application.
 *
 * Single-threaded main loop using poll() on the device fd and
 * ncurses nodelay getch() for keyboard input.  REPLAY (which blocks
 * in the kernel) is handled in a detached background thread.
 */

#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../module/echo_ioctl.h"
#include "echo_visualizer.h"
#include "echo_controller.h"

#define DEVICE_PATH "/dev/echo_robot"
#define POLL_TIMEOUT_MS 100

volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(void)
{
	int fd;
	struct sigaction sa;
	struct echo_snapshot snap;
	struct pollfd pfd;
	int ch;

	/* Install SIGINT handler */
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	/* Open the device */
	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " DEVICE_PATH);
		return EXIT_FAILURE;
	}

	/* Seed the snapshot with initial state via ioctl */
	memset(&snap, 0, sizeof(snap));
	ioctl(fd, ECHO_IOC_GET_STATE, &snap);

	/* Init ncurses */
	echo_visualizer_init();

	/* Main loop */
	while (running) {
		/* Render current state */
		echo_visualizer_render(&snap, echo_controller_get_status());

		/* Poll device fd for new data */
		pfd.fd = fd;
		pfd.events = POLLIN;

		if (poll(&pfd, 1, POLL_TIMEOUT_MS) > 0) {
			if (pfd.revents & POLLIN) {
				ssize_t n = read(fd, &snap, sizeof(snap));

				if (n < 0 && errno == EINTR)
					break;
			}
		}

		/* Check for keyboard input (non-blocking) */
		while ((ch = getch()) != ERR) {
			if (echo_controller_handle_key(fd, ch) < 0)
				break;
		}
	}

	echo_visualizer_cleanup();
	close(fd);

	fprintf(stderr, "Shutdown complete.\n");
	return EXIT_SUCCESS;
}
