/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_CHARDEV_H
#define ECHO_CHARDEV_H

struct echo_device;

int echo_chardev_init(struct echo_device *dev);
void echo_chardev_cleanup(struct echo_device *dev);

#endif /* ECHO_CHARDEV_H */
