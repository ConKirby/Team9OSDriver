/* SPDX-License-Identifier: GPL-2.0 */
/*
 * echo_servo.h — I2C / PCA9685 servo subsystem (leaf — no dependencies)
 */
#ifndef ECHO_SERVO_H
#define ECHO_SERVO_H

#include <linux/types.h>

struct echo_servo_ctx;

struct echo_servo_ctx *echo_servo_create(bool sim_mode);
void echo_servo_destroy(struct echo_servo_ctx *ctx);

int echo_servo_set_angle(struct echo_servo_ctx *ctx, u8 servo_id, u16 angle);
u16 echo_servo_get_angle(struct echo_servo_ctx *ctx, u8 servo_id);

#endif /* ECHO_SERVO_H */
