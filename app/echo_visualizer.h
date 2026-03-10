/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_VISUALIZER_H
#define ECHO_VISUALIZER_H

#include "../module/echo_ioctl.h"

void echo_visualizer_init(void);
void echo_visualizer_render(const struct echo_snapshot *snap,
			    const char *status_msg);
void echo_visualizer_cleanup(void);

#endif /* ECHO_VISUALIZER_H */
