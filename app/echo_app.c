//echo_app.c - Main entry point for the Project Echo user-space application.

// Multi-threaded: a dedicated reader thread blocks on read() until the kernel
// signals new data, while the main thread handles ncurses rendering and input.
// REPLAY (which blocks in the kernel) is handled in a detached background thread.


#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Header file imports
// icotl is the join between the kernel and user space
#include "../module/echo_ioctl.h"
#include "echo_visualizer.h"
#include "echo_controller.h"

#define DEVICE_PATH "/dev/echo_robot"
// Main loop refresh rate (~10 Hz)
#define REFRESH_MS 100

// volatile forces re-read from memory (not a register); sig_atomic_t guarantees atomic access.
volatile sig_atomic_t running = 1;

// Shared snapshot protected by mutex so the reader thread and main thread
// can safely access it concurrently.
static struct echo_snapshot shared_snap;
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;

// Sets flag instead of exit() because exit() is not async-signal-safe and would skip cleanup.
static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

// Reader thread — blocks on read() until the kernel wakes wq_read with new data.
// This is the blocking call that satisfies the brief's requirement.
static void *reader_thread(void *arg)
{
	int fd = (int)(long)arg;
	struct echo_snapshot snap;

	while (running) {
		ssize_t n = read(fd, &snap, sizeof(snap));

		if (n < 0)
			break;

		pthread_mutex_lock(&snap_lock);
		shared_snap = snap;
		pthread_mutex_unlock(&snap_lock);
	}

	return NULL;
}

int main(void)
{
	int fd;
	struct sigaction sa;
	// local copy for rendering
	struct echo_snapshot snap;
	pthread_t reader_tid;
	int ch;

	sa.sa_handler = sigint_handler;
	// dont block other signals while handling this one
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	// Open the device — no O_NONBLOCK so read() blocks in the kernel
	fd = open(DEVICE_PATH, O_RDWR);

	if (fd < 0) {
		perror("open " DEVICE_PATH);
		return EXIT_FAILURE;
	}

	// Seed the snapshot with initial state via ioctl
	memset(&shared_snap, 0, sizeof(shared_snap));
	ioctl(fd, ECHO_IOC_GET_STATE, &shared_snap);

	// Spawn reader thread — its read() sleeps in kernel until new data arrives
	pthread_create(&reader_tid, NULL, reader_thread, (void *)(long)fd);

	// Init ncurses
	echo_visualizer_init();

	// Main loop
	while (running) {
		// Grab latest snapshot from reader thread
		pthread_mutex_lock(&snap_lock);
		snap = shared_snap;
		pthread_mutex_unlock(&snap_lock);

		// Render current state
		echo_visualizer_render(&snap, echo_controller_get_status());

		// Check for keyboard input (non-blocking via ncurses nodelay)
		while ((ch = getch()) != ERR) {
			if (echo_controller_handle_key(fd, ch) < 0)
				break;
		}

		// ~10 Hz refresh rate
		napms(REFRESH_MS);
	}

	echo_visualizer_cleanup();

	// Clean up reader thread (read() is a cancellation point)
	pthread_cancel(reader_tid);
	pthread_join(reader_tid, NULL);

	close(fd);

	fprintf(stderr, "Shutdown complete.\n");
	return EXIT_SUCCESS;
}
