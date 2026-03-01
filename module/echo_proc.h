/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_PROC_H
#define ECHO_PROC_H

struct echo_device;

int echo_proc_init(struct echo_device *dev);
void echo_proc_cleanup(struct echo_device *dev);

#endif /* ECHO_PROC_H */
