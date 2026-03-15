#ifndef ECHO_CONTROLLER_H
#define ECHO_CONTROLLER_H

// Process a single keypress. Returns 0 normally, -1 if quit requested.
int echo_controller_handle_key(int fd, int ch);

// Get the last status message (for display by the visualizer).
const char *echo_controller_get_status(void);

#endif /* ECHO_CONTROLLER_H */
