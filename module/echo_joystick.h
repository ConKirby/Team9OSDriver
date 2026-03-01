/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_JOYSTICK_H
#define ECHO_JOYSTICK_H

struct echo_device;

int echo_joystick_init(struct echo_device *dev);
void echo_joystick_cleanup(struct echo_device *dev);

#endif /* ECHO_JOYSTICK_H */
