//echo_visualizer.c - ncurses rendering for Project Echo.

// Provides init/render/cleanup functions called from the main loop.
// All ncurses calls happen in the main thread.

#include <ncurses.h>
#include <string.h>

#include "../module/echo_ioctl.h"
#include "echo_visualizer.h"

// our kernel fifo holds up to 256 entries
#define ECHO_FIFO_SIZE 256

static const char *mode_name(uint32_t mode)
{
	// converts numeric mode value back to string
	switch (mode) {
	case ECHO_MODE_VAL_IDLE:   return "IDLE";
	case ECHO_MODE_VAL_TEACH:  return "TEACH";
	case ECHO_MODE_VAL_REPLAY: return "REPLAY";
	default:                   return "UNKNOWN";
	}
}

static void format_bar(char *buf, size_t buf_sz, int filled, int bar_len)
{
	int i;
	// prevents out of bounds writes
	if (filled > bar_len){
		filled = bar_len;
	}
	if (filled < 0){
		filled = 0;
	}

	// format string to start with []
	buf[0] = '[';
	// iterate through length of the list and fill with either # or -
	for (i = 0; i < bar_len; i++){
		buf[1 + i] = (i < filled) ? '#' : '-';
	}
	// last char is a closed ]
	buf[1 + bar_len] = ']';
	// terminate string
	buf[2 + bar_len] = '\0';
	// Suppresses "unused parameter" warning
	(void)buf_sz;
}

void echo_visualizer_init(void)
{
	// Initialises the ncurses library
	initscr();
	// Disables line buffering, each key press is avaibalble immediately
	cbreak();
	// Don't display typed characters on screen
	noecho();
	// Hide blinking cursor
	curs_set(0);
	// Makes `getch()` non-blocking
	nodelay(stdscr, TRUE);
}

// const as we are reading and not writing
void echo_visualizer_render(const struct echo_snapshot *snap, const char *status_msg)
{
	char pan_bar[32];
	char tilt_bar[32];
	char tilt2_bar[32];
	// width of character graph
	int bar_len = 18;
	int row;

	// Angle 0 → `(0 * 18) / 180` = 0 filled chars → `[------------------]`
	// Angle 90 → `(90 * 18) / 180` = 9 filled chars → `[#########---------]`
	// Angle 180 → `(180 * 18) / 180` = 18 filled chars → `[##################]`
	format_bar(pan_bar, sizeof(pan_bar), (snap->pan_angle * bar_len) / 180, bar_len);
	format_bar(tilt_bar, sizeof(tilt_bar), (snap->tilt_angle * bar_len) / 180, bar_len);
	format_bar(tilt2_bar, sizeof(tilt2_bar), (snap->tilt2_angle * bar_len) / 180, bar_len);

	// Clears the virtual screen buffer
	erase();

	row = 1;
	// move the cursor to that row,column and then write
	mvprintw(row++, 2, "+------------------------------------+");
	mvprintw(row++, 2, "|       PROJECT ECHO STATUS          |");
	mvprintw(row++, 2, "+------------------------------------+");
	mvprintw(row++, 2, "| Mode:    %-26s |", mode_name(snap->mode));
	mvprintw(row++, 2, "| Pan:     %s %3u  |", pan_bar, snap->pan_angle);
	mvprintw(row++, 2, "| Tilt:    %s %3u  |", tilt_bar, snap->tilt_angle);
	mvprintw(row++, 2, "| Tilt2:   %s %3u  |", tilt2_bar, snap->tilt2_angle);
	mvprintw(row++, 2, "| Buffer:  %-3u / %-21d |", snap->buffer_count,
		 ECHO_FIFO_SIZE);
	mvprintw(row++, 2, "| Moves:   %-26u |", snap->total_moves);
	mvprintw(row++, 2, "| Replays: %-26u |", snap->total_replays);
	mvprintw(row++, 2, "| IRQs:    %-26u |", snap->irq_count);
	mvprintw(row++, 2, "+------------------------------------+");

	row++;
	mvprintw(row++, 2, "Controls:");
	mvprintw(row++, 2, "  [1/t] Teach   [2/r] Replay   [3/s] Stop");
	mvprintw(row++, 2, "  [6] Reset     [q] Quit");

	row++;
	mvprintw(row++, 2, "Status: %s", status_msg ? status_msg : "");

	refresh();
}

// restore the terminal to its original state
void echo_visualizer_cleanup(void)
{
	endwin();
}
