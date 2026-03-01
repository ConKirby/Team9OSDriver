/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_SERVO_H
#define ECHO_SERVO_H

struct echo_device;

int echo_servo_init(struct echo_device *dev);
void echo_servo_cleanup(struct echo_device *dev);
int echo_servo_set_angle(struct echo_device *dev, u8 servo_id, u16 angle);
u16 echo_servo_get_angle(struct echo_device *dev, u8 servo_id);

#endif /* ECHO_SERVO_H */
