/*echo_main.c — Module entry point and subsystem coordinator
 * only file that knows how subsystems connect.
 * creates each subsystem, wires their ops callbacks, and tears everything down on exit.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "echo_main.h"
#include "echo_servo.h"
#include "echo_buffer.h"
#include "echo_state.h"
#include "echo_joystick.h"
#include "echo_chardev.h"
#include "echo_proc.h"

//module parameters 
static int gpio_up     = 529;  //BCM 17 + 512 offset on Pi 4
static int gpio_down   = 539;  //BCM 27 + 512 offset on Pi 4
static int gpio_left   = 534;  //BCM 22 + 512 offset on Pi 4
static int gpio_right  = 535;  //BCM 23 + 512 offset on Pi 4
module_param(gpio_up,     int, 0444);
module_param(gpio_down,   int, 0444);
module_param(gpio_left,   int, 0444);
module_param(gpio_right,  int, 0444);

MODULE_PARM_DESC(gpio_up,     "GPIO pin for UP direction");
MODULE_PARM_DESC(gpio_down,   "GPIO pin for DOWN direction");
MODULE_PARM_DESC(gpio_left,   "GPIO pin for LEFT direction");
MODULE_PARM_DESC(gpio_right,  "GPIO pin for RIGHT direction");

static unsigned long timeout_ms = ECHO_DEFAULT_TIMEOUT_MS;
module_param(timeout_ms, ulong, 0644);
MODULE_PARM_DESC(timeout_ms, "Inactivity timeout in milliseconds");

static bool sim_mode = true;
module_param(sim_mode, bool, 0444);
MODULE_PARM_DESC(sim_mode, "Simulation mode (no real hardware)");

//global device (single instance driver)
static struct echo_device *echo_dev;

//notification helpers
static void notify_readers(struct echo_device *dev)
{
	WRITE_ONCE(dev->new_data_avail, true);
	wake_up_interruptible(&dev->wq_read);
}
static void signal_replay_done(struct echo_device *dev)
{
	WRITE_ONCE(dev->replay_finished, true);
	wake_up_interruptible(&dev->wq_replay_done);
	notify_readers(dev);
}

//BUFFER OPS CALLBACKS— invoked by echo_buffer during replay
static int buf_op_move_servo(void *data, u8 servo_id, u16 angle)
{
	struct echo_device *dev = data;
	return echo_servo_set_angle(dev->servo, servo_id, angle);
}

static bool buf_op_should_stop(void *data)
{
	struct echo_device *dev = data;
	return echo_state_get_mode(dev->state) != ECHO_MODE_REPLAY;
}

static void buf_op_replay_done(void *data)
{
	struct echo_device *dev = data;
	echo_state_replay_complete(dev->state);
	signal_replay_done(dev);
}

static void buf_op_notify(void *data)
{
	notify_readers(data);
}

static const struct echo_buffer_ops buf_ops = {
	.move_servo  = buf_op_move_servo,
	.should_stop = buf_op_should_stop,
	.replay_done = buf_op_replay_done,
	.notify      = buf_op_notify,
};

//STATE OPS CALLBACKS — invoked by echo_state for cross-module actions
static int state_op_move_servo(void *data, u8 servo_id, u16 angle)
{
	struct echo_device *dev = data;
	return echo_servo_set_angle(dev->servo, servo_id, angle);
}

static u16 state_op_get_servo(void *data, u8 servo_id)
{
	struct echo_device *dev = data;
	return echo_servo_get_angle(dev->servo, servo_id);
}

static void state_op_record_move(void *data, const struct echo_move *move)
{
	struct echo_device *dev = data;
	echo_buffer_record(dev->buffer, move);
}

static void state_op_clear_buffer(void *data)
{
	struct echo_device *dev = data;
	echo_buffer_clear(dev->buffer);
}

static void state_op_start_replay(void *data)
{
	struct echo_device *dev = data;
	echo_buffer_start_replay(dev->buffer);
}

static void state_op_cancel_replay(void *data)
{
	struct echo_device *dev = data;
	echo_buffer_cancel_replay(dev->buffer);
	signal_replay_done(dev);
}

static void state_op_notify(void *data)
{
	notify_readers(data);
}

static const struct echo_state_ops state_ops = {
	.move_servo    = state_op_move_servo,
	.get_servo     = state_op_get_servo,
	.record_move   = state_op_record_move,
	.clear_buffer  = state_op_clear_buffer,
	.start_replay  = state_op_start_replay,
	.cancel_replay = state_op_cancel_replay,
	.notify        = state_op_notify,
};

//JOYSTICK OPS CALLBACKS— invoked by echo_joystick on physical input
static void joy_op_direction(void *data, u8 servo_id, int delta)
{
	struct echo_device *dev = data;
	echo_state_handle_input(dev->state, servo_id, delta);
}

static const struct echo_joystick_ops joy_ops = {
	.on_direction = joy_op_direction,
};

//module init/exit
static int __init echo_init(void)
{
	int gpio_pins[ECHO_NUM_GPIO];

	//allocate coordinator struct
	echo_dev = kzalloc(sizeof(*echo_dev), GFP_KERNEL);
	if (!echo_dev)
		return -ENOMEM;

	echo_dev->sim_mode   = sim_mode;
	echo_dev->timeout_ms = timeout_ms;

	init_waitqueue_head(&echo_dev->wq_read);
	init_waitqueue_head(&echo_dev->wq_replay_done);

	//create servo (leaf — no deps)
	echo_dev->servo = echo_servo_create(sim_mode);
	if (IS_ERR(echo_dev->servo)) {
		pr_err("echo: servo create failed\n");
		goto err_free;
	}

	//create buffer (deps: buf_ops -> echo_dev)
	echo_dev->buffer = echo_buffer_create(&buf_ops, echo_dev);
	if (IS_ERR(echo_dev->buffer)) {
		pr_err("echo: buffer create failed\n");
		goto err_servo;
	}

	//create state machine (deps: state_ops -> echo_dev)
	echo_dev->state = echo_state_create(timeout_ms, &state_ops, echo_dev);
	if (IS_ERR(echo_dev->state)) {
		pr_err("echo: state create failed\n");
		goto err_buffer;
	}

	//create joystick (deps: joy_ops -> echo_dev)
	gpio_pins[ECHO_GPIO_UP]     = gpio_up;
	gpio_pins[ECHO_GPIO_DOWN]   = gpio_down;
	gpio_pins[ECHO_GPIO_LEFT]   = gpio_left;
	gpio_pins[ECHO_GPIO_RIGHT]  = gpio_right;

	echo_dev->joystick = echo_joystick_create(gpio_pins, sim_mode,
						  &joy_ops, echo_dev);
	if (IS_ERR(echo_dev->joystick)) {
		pr_err("echo: joystick create failed\n");
		goto err_state;
	}

	//create chardev (deps: echo_dev -> all subsystems)
	echo_dev->chardev = echo_chardev_create(echo_dev);
	if (IS_ERR(echo_dev->chardev)) {
		pr_err("echo: chardev create failed\n");
		goto err_joystick;
	}

	//create proc (deps: echo_dev -> all subsystems, read-only)
	echo_dev->proc = echo_proc_create(echo_dev);
	if (IS_ERR(echo_dev->proc)) {
		pr_err("echo: proc create failed\n");
		goto err_chardev;
	}

	pr_info("echo: module loaded (sim_mode=%d, timeout=%lu ms)\n",
		sim_mode, timeout_ms);
	return 0;

err_chardev:
	echo_chardev_destroy(echo_dev->chardev);
err_joystick:
	echo_joystick_destroy(echo_dev->joystick);
err_state:
	echo_state_destroy(echo_dev->state);
err_buffer:
	echo_buffer_destroy(echo_dev->buffer);
err_servo:
	echo_servo_destroy(echo_dev->servo);
err_free:
	kfree(echo_dev);
	echo_dev = NULL;
	return -ENODEV;
}

static void __exit echo_exit(void)
{
	//tear down in reverse init order
	echo_proc_destroy(echo_dev->proc);
	echo_chardev_destroy(echo_dev->chardev);
	echo_joystick_destroy(echo_dev->joystick);
	echo_state_destroy(echo_dev->state);
	echo_buffer_destroy(echo_dev->buffer);
	echo_servo_destroy(echo_dev->servo);

	kfree(echo_dev);
	echo_dev = NULL;

	pr_info("echo: module unloaded\n");
}

module_init(echo_init);
module_exit(echo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Team 9");
MODULE_DESCRIPTION("Project Echo — Pan/Tilt Servo Motion Recorder");