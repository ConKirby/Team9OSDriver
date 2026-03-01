/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ECHO_BUFFER_H
#define ECHO_BUFFER_H

struct echo_device;
struct echo_move;

int echo_buffer_init(struct echo_device *dev);
void echo_buffer_cleanup(struct echo_device *dev);
int echo_buffer_record(struct echo_device *dev, const struct echo_move *move);
int echo_buffer_get_next(struct echo_device *dev, struct echo_move *move);
void echo_buffer_clear(struct echo_device *dev);
unsigned int echo_buffer_count(struct echo_device *dev);
void echo_buffer_replay_worker(struct work_struct *work);

#endif /* ECHO_BUFFER_H */
