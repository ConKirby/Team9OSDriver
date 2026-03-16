/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_proc.h — /proc/echo_stats subsystem
 *
 * Read-only view of all device statistics.
 * Holds a back-pointer to echo_device for querying all subsystems. 
 */
#ifndef ECHO_PROC_H
#define ECHO_PROC_H

struct echo_proc_ctx;
struct echo_device;

struct echo_proc_ctx *echo_proc_create(struct echo_device *dev);
void echo_proc_destroy(struct echo_proc_ctx *ctx);

#endif /* ECHO_PROC_H */
