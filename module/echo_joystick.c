// SPDX-License-Identifier: GPL-2.0
/*
 * echo_joystick.c — GPIO joystick input with threaded IRQ handlers
 *
 * Reads a 4-way joystick (common pin to GND, direction pins with
 * internal pull-ups, active-low) via GPIO interrupts and fires ops
 * callbacks for direction presses.
 *
 * Dependencies: echo_joystick_ops callbacks only (wired by echo_main.c).
 */

#include <linux/atomic.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "echo_types.h"
#include "echo_joystick.h"

/* ── Private context ───────────────────────────────────────────────── */
struct echo_joystick_ctx {
	int gpio_pins[ECHO_NUM_GPIO];
	int irqs[ECHO_NUM_GPIO];
	unsigned long last_irq_jiffies[ECHO_NUM_GPIO];
	spinlock_t irq_lock;  /* protects last_irq_jiffies */
	bool sim_mode;

	const struct echo_joystick_ops *ops;
	void *ops_data;

	atomic_t irq_count;
};

/* ── GPIO names for request_threaded_irq ───────────────────────────── */

static const char * const gpio_names[ECHO_NUM_GPIO] = {
	"echo_joy_up",
	"echo_joy_down",
	"echo_joy_left",
	"echo_joy_right",
};

/* ── IRQ handlers ──────────────────────────────────────────────────── */

static irqreturn_t joystick_hardirq(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t joystick_thread_fn(int irq, void *data)
{
	struct echo_joystick_ctx *ctx = data;
	int pin_idx = -1;
	int i;

	/* Identify which GPIO pin fired */
	for (i = 0; i < ECHO_NUM_GPIO; i++) {
		if (irq == ctx->irqs[i]) {
			pin_idx = i;
			break;
		}
	}
	if (pin_idx < 0)
		return IRQ_HANDLED;

	/* Debounce */
	spin_lock(&ctx->irq_lock);
	if (jiffies - ctx->last_irq_jiffies[pin_idx] <
	    msecs_to_jiffies(ECHO_DEBOUNCE_MS))
		spin_unlock(&ctx->irq_lock);
		return IRQ_HANDLED;

	ctx->last_irq_jiffies[pin_idx] = jiffies;
	spin_unlock(&ctx->irq_lock);
	atomic_inc(&ctx->irq_count);

	/* Map pin to callback */
	switch (pin_idx) {
	case ECHO_GPIO_UP:
		ctx->ops->on_direction(ctx->ops_data,
				       ECHO_SERVO_TILT, +ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_DOWN:
		ctx->ops->on_direction(ctx->ops_data,
				       ECHO_SERVO_TILT, -ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_LEFT:
		ctx->ops->on_direction(ctx->ops_data,
				       ECHO_SERVO_PAN, -ECHO_ANGLE_STEP);
		break;
	case ECHO_GPIO_RIGHT:
		ctx->ops->on_direction(ctx->ops_data,
				       ECHO_SERVO_PAN, +ECHO_ANGLE_STEP);
		break;
	default:
		break;
	}

	return IRQ_HANDLED;
}

/* ── Public API ────────────────────────────────────────────────────── */

struct echo_joystick_ctx *echo_joystick_create(
		const int gpio_pins[ECHO_NUM_GPIO],
		bool sim_mode,
		const struct echo_joystick_ops *ops,
		void *ops_data)
{
	struct echo_joystick_ctx *ctx;
	int i, ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->sim_mode = sim_mode;
	ctx->ops      = ops;
	ctx->ops_data = ops_data;
	atomic_set(&ctx->irq_count, 0);
	spin_lock_init(&ctx->irq_lock);

	for (i = 0; i < ECHO_NUM_GPIO; i++)
		ctx->gpio_pins[i] = gpio_pins[i];

	if (sim_mode) {
		pr_info("echo: joystick: simulation mode (no GPIO)\n");
		return ctx;
	}

	for (i = 0; i < ECHO_NUM_GPIO; i++) {
		ret = gpio_request(ctx->gpio_pins[i], gpio_names[i]);
		if (ret) {
			pr_err("echo: joystick: GPIO %d request failed (%d)\n",
			       ctx->gpio_pins[i], ret);
			goto err_gpio;
		}

		ret = gpio_direction_input(ctx->gpio_pins[i]);
		if (ret) {
			pr_err("echo: joystick: GPIO %d set input failed (%d)\n",
			       ctx->gpio_pins[i], ret);
			gpio_free(ctx->gpio_pins[i]);
			goto err_gpio;
		}

		ctx->irqs[i] = gpio_to_irq(ctx->gpio_pins[i]);
		if (ctx->irqs[i] < 0) {
			pr_err("echo: joystick: GPIO %d to IRQ failed (%d)\n",
			       ctx->gpio_pins[i], ctx->irqs[i]);
			ret = ctx->irqs[i];
			gpio_free(ctx->gpio_pins[i]);
			goto err_gpio;
		}

		ret = request_threaded_irq(ctx->irqs[i],
					   joystick_hardirq,
					   joystick_thread_fn,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   gpio_names[i], ctx);
		if (ret) {
			pr_err("echo: joystick: IRQ %d request failed (%d)\n",
			       ctx->irqs[i], ret);
			gpio_free(ctx->gpio_pins[i]);
			goto err_gpio;
		}
	}

	pr_info("echo: joystick: initialised (%d GPIO pins)\n", ECHO_NUM_GPIO);
	return ctx;

err_gpio:
	while (--i >= 0) {
		free_irq(ctx->irqs[i], ctx);
		gpio_free(ctx->gpio_pins[i]);
	}
	kfree(ctx);
	return ERR_PTR(ret);
}

void echo_joystick_destroy(struct echo_joystick_ctx *ctx)
{
	int i;

	if (!ctx)
		return;

	if (!ctx->sim_mode) {
		for (i = ECHO_NUM_GPIO - 1; i >= 0; i--) {
			free_irq(ctx->irqs[i], ctx);
			gpio_free(ctx->gpio_pins[i]);
		}
	}

	kfree(ctx);
}

int echo_joystick_get_irq_count(struct echo_joystick_ctx *ctx)
{
	return atomic_read(&ctx->irq_count);
}
