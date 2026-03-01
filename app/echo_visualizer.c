/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_visualizer.c - ncurses UI thread for Project Echo.
 *
 * Performs a blocking read() of the device state snapshot.  The kernel
 * driver wakes the wait queue whenever new data is available, so this
 * thread sleeps efficiently until there is something to display.
 */

#include <errno.h>
#include <ncurses.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "echo_visualizer.h"

/* Import the global shutdown flag from echo_app.c */
extern volatile sig_atomic_t running;

/*
 * User-space compatible copy of struct echo_state_snapshot.
 * The kernel version uses u32/u16 types which are not available here,
 * so we mirror the layout with fixed-width stdint types.
 */
struct echo_state_snapshot {
	uint32_t mode;
	uint16_t pan_angle;
	uint16_t tilt_angle;
	uint32_t buffer_count;
	uint32_t total_moves;
	uint32_t total_replays;
};

/* FIFO capacity — must match ECHO_FIFO_SIZE in echo_main.h */
#define ECHO_FIFO_SIZE 256

/* ------------------------------------------------------------------ */

static const char *mode_name(uint32_t mode)
{
	switch (mode) {
	case 0:  return "IDLE";
	case 1:  return "TEACH";
	case 2:  return "REPLAY";
	default: return "UNKNOWN";
	}
}

/*
 * Build a simple horizontal bar.
 *   filled = number of '#' characters (0..bar_len)
 */
static void format_bar(char *buf, size_t buf_sz, int filled, int bar_len)
{
	int i;

	if (filled > bar_len)
		filled = bar_len;
	if (filled < 0)
		filled = 0;

	buf[0] = '[';
	for (i = 0; i < bar_len; i++)
		buf[1 + i] = (i < filled) ? '#' : '-';
	buf[1 + bar_len] = ']';
	buf[2 + bar_len] = '\0';
	(void)buf_sz; /* silence unused warning */
}

/* ------------------------------------------------------------------ */

void *echo_visualizer_run(void *arg)
{
	int fd = (int)(long)arg;
	struct echo_state_snapshot snap;
	ssize_t n;
	char pan_bar[32];
	char tilt_bar[32];
	int bar_len = 10;
	int row;

	/* Initialise ncurses */
	initscr();
	cbreak();
	noecho();
	curs_set(0);
	nodelay(stdscr, FALSE);

	while (running) {
		/* Blocking read — sleeps until the kernel signals new data */
		n = read(fd, &snap, sizeof(snap));

		if (n < 0) {
			if (errno == EINTR)
				break;   /* interrupted by signal — time to exit */
			/* Transient error — try again */
			continue;
		}

		if ((size_t)n != sizeof(snap))
			continue;   /* short read — skip this frame */

		/* Build progress bars (0-180 mapped to 0-bar_len) */
		format_bar(pan_bar,  sizeof(pan_bar),
			   (snap.pan_angle  * bar_len) / 180, bar_len);
		format_bar(tilt_bar, sizeof(tilt_bar),
			   (snap.tilt_angle * bar_len) / 180, bar_len);

		/* Render the dashboard */
		erase();

		row = 1;
		mvprintw(row++, 2, "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97");
		mvprintw(row++, 2, "\xe2\x95\x91       PROJECT ECHO STATUS         \xe2\x95\x91");
		mvprintw(row++, 2, "\xe2\x95\xa0\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa3");
		mvprintw(row++, 2, "\xe2\x95\x91 Mode:    %-26s\xe2\x95\x91", mode_name(snap.mode));
		mvprintw(row++, 2, "\xe2\x95\x91 Pan:     %s %3u\xc2\xb0%*s\xe2\x95\x91",
			 pan_bar, snap.pan_angle,
			 (int)(24 - strlen(pan_bar) - 5), "");
		mvprintw(row++, 2, "\xe2\x95\x91 Tilt:    %s %3u\xc2\xb0%*s\xe2\x95\x91",
			 tilt_bar, snap.tilt_angle,
			 (int)(24 - strlen(tilt_bar) - 5), "");
		mvprintw(row++, 2, "\xe2\x95\x91 Buffer:  %-3u / %-21d\xe2\x95\x91",
			 snap.buffer_count, ECHO_FIFO_SIZE);
		mvprintw(row++, 2, "\xe2\x95\x91 Moves:   %-26u\xe2\x95\x91", snap.total_moves);
		mvprintw(row++, 2, "\xe2\x95\x91 Replays: %-26u\xe2\x95\x91", snap.total_replays);
		mvprintw(row++, 2, "\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d");

		mvprintw(row + 1, 2, "Press Ctrl-C to quit.");

		refresh();
	}

	endwin();
	return NULL;
}
