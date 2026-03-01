/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_app.c - Main entry point for the Project Echo user-space application.
 *
 * Opens /dev/echo_robot and spawns two threads:
 *   1. Visualizer  — ncurses UI driven by blocking read()
 *   2. Controller  — menu-driven ioctl/write interface
 *
 * SIGINT sets a global flag so both threads can exit gracefully.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "echo_visualizer.h"
#include "echo_controller.h"

#define DEVICE_PATH "/dev/echo_robot"

/* Global shutdown flag — set by the SIGINT handler */
volatile sig_atomic_t running = 1;

/* ------------------------------------------------------------------ */
static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

static void print_usage(void)
{
	fprintf(stderr,
		"==========================================================\n"
		"  Project Echo — User-Space Control Application\n"
		"==========================================================\n"
		"\n"
		"  Device : %s\n"
		"  Threads: visualizer (ncurses) + controller (menu)\n"
		"\n"
		"  The visualizer performs blocking read() on the device and\n"
		"  renders the robot state with ncurses.\n"
		"\n"
		"  The controller accepts keyboard commands via stderr/stdin\n"
		"  and issues write()/ioctl() calls to drive the robot.\n"
		"\n"
		"  Press Ctrl-C to quit.\n"
		"==========================================================\n\n",
		DEVICE_PATH);
}

/* ------------------------------------------------------------------ */
int main(void)
{
	int fd;
	pthread_t vis_thread, ctrl_thread;
	struct sigaction sa;
	int ret;

	print_usage();

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

	fprintf(stderr, "[echo_app] Device opened: fd %d\n", fd);

	/* Spawn the visualizer thread */
	ret = pthread_create(&vis_thread, NULL, echo_visualizer_run,
			     (void *)(long)fd);
	if (ret != 0) {
		fprintf(stderr, "[echo_app] Failed to create visualizer thread: %s\n",
			strerror(ret));
		close(fd);
		return EXIT_FAILURE;
	}

	/* Spawn the controller thread */
	ret = pthread_create(&ctrl_thread, NULL, echo_controller_run,
			     (void *)(long)fd);
	if (ret != 0) {
		fprintf(stderr, "[echo_app] Failed to create controller thread: %s\n",
			strerror(ret));
		running = 0;
		pthread_join(vis_thread, NULL);
		close(fd);
		return EXIT_FAILURE;
	}

	/* Wait for both threads to finish */
	pthread_join(vis_thread, NULL);
	pthread_join(ctrl_thread, NULL);

	close(fd);
	fprintf(stderr, "[echo_app] Shutdown complete.\n");
	return EXIT_SUCCESS;
}
