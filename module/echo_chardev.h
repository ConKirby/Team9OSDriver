

// echo_chardev.h — Character device subsystem (/dev/echo_robot)
// The chardev is the user-space gateway. It holds a back-pointer to
// echo_device and calls all subsystem APIs through it.

#ifndef ECHO_CHARDEV_H
#define ECHO_CHARDEV_H

struct echo_chardev_ctx;
struct echo_device;

struct echo_chardev_ctx *echo_chardev_create(struct echo_device *dev);
void echo_chardev_destroy(struct echo_chardev_ctx *ctx);

int echo_chardev_get_open_count(struct echo_chardev_ctx *ctx);

#endif
