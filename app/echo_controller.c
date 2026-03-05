/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_controller.c - Menu-driven control thread for Project Echo.
 *
 * Reads single-character commands from stdin and translates them into
 * write() (for echo_cmd) or ioctl() calls on the device fd.
 *
 * Note: ncurses owns stdout, so all output here goes to stderr.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../module/echo_ioctl.h"
#include "echo_controller.h"

/* Import the global shutdown flag from echo_app.c */
extern volatile sig_atomic_t running;

/* ------------------------------------------------------------------ */

static void print_menu(void)
{
	fprintf(stderr,
		"\n"
		"[1] Teach Mode  [2] Replay (blocking)  [3] Stop  [4] Move Servo\n"
		"[5] Get State   [6] Reset              [7] Set Speed  [q] Quit\n"
		"> ");
}

static void do_teach(int fd)
{
	struct echo_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.command = ECHO_CMD_TEACH;

	if (write(fd, &cmd, sizeof(cmd)) < 0)
		perror("[controller] write(TEACH)");
	else
		fprintf(stderr, "[controller] Entered TEACH mode.\n");
}

static void do_replay(int fd)
{
	struct echo_cmd cmd;
	struct timespec t_start, t_end;
	double elapsed;

	memset(&cmd, 0, sizeof(cmd));
	cmd.command = ECHO_CMD_REPLAY;

	/* Timestamp before the blocking write */
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	fprintf(stderr, "[controller] Requesting REPLAY at ");
	{
		struct timespec t_wall;
		struct tm tm_buf;

		clock_gettime(CLOCK_REALTIME, &t_wall);
		localtime_r(&t_wall.tv_sec, &tm_buf);
		fprintf(stderr, "%02d:%02d:%02d.%03ld\n",
			tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
			t_wall.tv_nsec / 1000000);
	}

	/* This write() blocks until the kernel replay work finishes */
	if (write(fd, &cmd, sizeof(cmd)) < 0) {
		perror("[controller] write(REPLAY)");
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	elapsed = (t_end.tv_sec  - t_start.tv_sec)
		+ (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

	fprintf(stderr, "[controller] Replay finished. Blocked for %.2f seconds.\n",
		elapsed);
}

static void do_stop(int fd)
{
	struct echo_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.command = ECHO_CMD_STOP;

	if (write(fd, &cmd, sizeof(cmd)) < 0)
		perror("[controller] write(STOP)");
	else
		fprintf(stderr, "[controller] Sent STOP.\n");
}

static void do_move(int fd)
{
	struct echo_cmd cmd;
	unsigned int servo_id, angle;

	fprintf(stderr, "  Servo ID (0=pan, 1=tilt): ");
	if (scanf("%u", &servo_id) != 1) {
		fprintf(stderr, "  Invalid input.\n");
		/* Flush the rest of the line */
		while (getchar() != '\n')
			;
		return;
	}

	fprintf(stderr, "  Angle (0-180): ");
	if (scanf("%u", &angle) != 1) {
		fprintf(stderr, "  Invalid input.\n");
		while (getchar() != '\n')
			;
		return;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.command  = ECHO_CMD_MOVE;
	cmd.servo_id = servo_id;
	cmd.angle    = angle;

	if (write(fd, &cmd, sizeof(cmd)) < 0)
		perror("[controller] write(MOVE)");
	else
		fprintf(stderr, "[controller] Moved servo %u to %u degrees.\n",
			servo_id, angle);
}

static void do_get_state(int fd)
{
	struct echo_snapshot st;

	memset(&st, 0, sizeof(st));

	if (ioctl(fd, ECHO_IOC_GET_STATE, &st) < 0) {
		perror("[controller] ioctl(GET_STATE)");
		return;
	}

	fprintf(stderr,
		"  mode         : %u\n"
		"  pan_angle    : %u\n"
		"  tilt_angle   : %u\n"
		"  buffer_count : %u\n"
		"  total_moves  : %u\n"
		"  total_replays: %u\n"
		"  irq_count    : %u\n",
		st.mode, st.pan_angle, st.tilt_angle,
		st.buffer_count, st.total_moves,
		st.total_replays, st.irq_count);
}

static void do_reset(int fd)
{
	if (ioctl(fd, ECHO_IOC_RESET) < 0)
		perror("[controller] ioctl(RESET)");
	else
		fprintf(stderr, "[controller] Device reset.\n");
}

static void do_set_speed(int fd)
{
	unsigned int speed;
	__u16 spd;

	fprintf(stderr, "  Speed multiplier (1=normal): ");
	if (scanf("%u", &speed) != 1) {
		fprintf(stderr, "  Invalid input.\n");
		while (getchar() != '\n')
			;
		return;
	}

	spd = (uint16_t)speed;
	if (ioctl(fd, ECHO_IOC_SET_SPEED, &spd) < 0)
		perror("[controller] ioctl(SET_SPEED)");
	else
		fprintf(stderr, "[controller] Speed set to %u.\n", speed);
}

/* ------------------------------------------------------------------ */

void *echo_controller_run(void *arg)
{
	int fd = (int)(long)arg;
	int ch;

	while (running) {
		print_menu();

		ch = getchar();
		if (ch == EOF)
			break;

		/* Consume trailing newline if present */
		if (ch != '\n') {
			int tmp = getchar();
			(void)tmp;
		}

		switch (ch) {
		case '1':
			do_teach(fd);
			break;
		case '2':
			do_replay(fd);
			break;
		case '3':
			do_stop(fd);
			break;
		case '4':
			do_move(fd);
			break;
		case '5':
			do_get_state(fd);
			break;
		case '6':
			do_reset(fd);
			break;
		case '7':
			do_set_speed(fd);
			break;
		case 'q':
		case 'Q':
			fprintf(stderr, "[controller] Quit requested.\n");
			running = 0;
			break;
		case '\n':
			/* Ignore bare enter */
			break;
		default:
			fprintf(stderr, "[controller] Unknown option '%c'\n", ch);
			break;
		}
	}

	return NULL;
}
