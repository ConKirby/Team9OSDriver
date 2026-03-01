// SPDX-License-Identifier: GPL-2.0
/*
 * echo_joystick.c — GPIO joystick input with threaded IRQ handlers
 *
 * Reads five-way joystick (up/down/left/right/button) via GPIO interrupts
 * and translates physical input into servo movements and mode changes.
 */

#include "echo_main.h"
#include "echo_joystick.h"
#include "echo_state.h"

#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>

/* ---------- IRQ handlers ---------- */

static irqreturn_t joystick_hardirq(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t joystick_thread_fn(int irq, void *data)
{
	struct echo_device *dev = data;
	int pin_idx = -1;
	int i;
	enum echo_mode cur_mode;

	/* Identify which GPIO pin fired */
	for (i = 0; i < ECHO_NUM_GPIO; i++) {
		if (irq == dev->irqs[i]) {
			pin_idx = i;
			break;
		}
	}
	if (pin_idx < 0)
		return IRQ_HANDLED;

	/* Debounce: ignore if too soon after the last interrupt on this pin */
	if (jiffies - dev->last_irq_jiffies[pin_idx] <
	    msecs_to_jiffies(ECHO_DEBOUNCE_MS))
		return IRQ_HANDLED;

	dev->last_irq_jiffies[pin_idx] = jiffies;
	atomic_inc(&dev->stat_irq_count);

	/* Map the pin to the appropriate action */
	switch (pin_idx) {
	case ECHO_GPIO_UP:
		echo_state_handle_input(dev, ECHO_SERVO_TILT, +ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_DOWN:
		echo_state_handle_input(dev, ECHO_SERVO_TILT, -ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_LEFT:
		echo_state_handle_input(dev, ECHO_SERVO_PAN, -ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_RIGHT:
		echo_state_handle_input(dev, ECHO_SERVO_PAN, +ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_BUTTON:
		/* Toggle mode: IDLE/TEACH -> TEACH, REPLAY -> stop */
		spin_lock(&dev->mode_lock);
		cur_mode = dev->mode;
		spin_unlock(&dev->mode_lock);

		if (cur_mode == ECHO_MODE_IDLE ||
		    cur_mode == ECHO_MODE_TEACH)
			echo_state_set_mode(dev, ECHO_MODE_TEACH);
		else if (cur_mode == ECHO_MODE_REPLAY)
			echo_state_stop(dev);
		break;
	default:
		break;
	}

	return IRQ_HANDLED;
}

/* ---------- Names for each GPIO request ---------- */

static const char * const gpio_names[ECHO_NUM_GPIO] = {
	"echo_joy_up",
	"echo_joy_down",
	"echo_joy_left",
	"echo_joy_right",
	"echo_joy_button",
};

/* ---------- Init / Cleanup ---------- */

int echo_joystick_init(struct echo_device *dev)
{
	int i, ret;

	if (dev->sim_mode) {
		pr_info("echo: Joystick: simulation mode (no GPIO)\n");
		return 0;
	}

	for (i = 0; i < ECHO_NUM_GPIO; i++) {
		ret = gpio_request(dev->gpio_pins[i], gpio_names[i]);
		if (ret) {
			pr_err("echo: Failed to request GPIO %d (%s): %d\n",
			       dev->gpio_pins[i], gpio_names[i], ret);
			goto err_gpio;
		}

		ret = gpio_direction_input(dev->gpio_pins[i]);
		if (ret) {
			pr_err("echo: Failed to set GPIO %d as input: %d\n",
			       dev->gpio_pins[i], ret);
			gpio_free(dev->gpio_pins[i]);
			goto err_gpio;
		}

		dev->irqs[i] = gpio_to_irq(dev->gpio_pins[i]);
		if (dev->irqs[i] < 0) {
			pr_err("echo: Failed to get IRQ for GPIO %d: %d\n",
			       dev->gpio_pins[i], dev->irqs[i]);
			ret = dev->irqs[i];
			gpio_free(dev->gpio_pins[i]);
			goto err_gpio;
		}

		ret = request_threaded_irq(dev->irqs[i],
					   joystick_hardirq,
					   joystick_thread_fn,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   gpio_names[i], dev);
		if (ret) {
			pr_err("echo: Failed to request IRQ %d for GPIO %d: %d\n",
			       dev->irqs[i], dev->gpio_pins[i], ret);
			gpio_free(dev->gpio_pins[i]);
			goto err_gpio;
		}
	}

	pr_info("echo: Joystick initialised (%d GPIO pins)\n", ECHO_NUM_GPIO);
	return 0;

err_gpio:
	/* Clean up already-initialised pins (indices 0 .. i-1) */
	while (--i >= 0) {
		free_irq(dev->irqs[i], dev);
		gpio_free(dev->gpio_pins[i]);
	}
	return ret;
}

void echo_joystick_cleanup(struct echo_device *dev)
{
	int i;

	if (dev->sim_mode)
		return;

	for (i = ECHO_NUM_GPIO - 1; i >= 0; i--) {
		free_irq(dev->irqs[i], dev);
		gpio_free(dev->gpio_pins[i]);
	}

	pr_info("echo: Joystick cleaned up\n");
}
