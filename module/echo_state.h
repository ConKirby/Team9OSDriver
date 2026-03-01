/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_STATE_H
#define ECHO_STATE_H

struct echo_device;

int echo_state_init(struct echo_device *dev);
void echo_state_cleanup(struct echo_device *dev);
void echo_state_handle_input(struct echo_device *dev, u8 servo_id, int delta);
int echo_state_start_replay(struct echo_device *dev);
void echo_state_stop(struct echo_device *dev);
int echo_state_set_mode(struct echo_device *dev, enum echo_mode new_mode);

#endif /* ECHO_STATE_H */
