// SPDX-License-Identifier: GPL-2.0
/*
 * echo_servo.c — Servo control subsystem (PCA9685 + I2C)
 *
 * This file implements the low-level hardware driver responsible for
 * controlling the robot’s servos. It communicates with a PCA9685
 * 16-channel PWM controller over the Linux I²C subsystem.
 *
 * Hardware overview
 * -----------------
 * The PCA9685 is a PWM generator commonly used for driving servos.
 * It produces 16 independent PWM channels using a 12-bit counter.
 *
 * Each servo expects a pulse every 20 ms (~50 Hz):
 *
 *    1.0 ms pulse → minimum angle
 *    1.5 ms pulse → centre position
 *    2.0 ms pulse → maximum angle
 *
 * The PCA9685 converts these pulse widths into "tick" values within
 * a 4096-step PWM cycle.
 *
 * This subsystem:
 *   • Converts servo angles (0–180°) into PWM tick values
 *   • Programs those values into the PCA9685 registers via I²C
 *   • Tracks the current servo position in software
 *
 * Simulation mode
 * ---------------
 * When sim_mode is enabled, all I²C communication is skipped.
 * The driver simply logs movements and updates internal state.
 * This allows development and testing without physical hardware.
 *
 * Design note
 * -----------
 * This is a "leaf subsystem": it does not depend on any other Echo
 * driver components. Higher-level modules call this API to move
 * servos without needing to know about I²C or PCA9685 details.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "echo_types.h"
#include "echo_servo.h"


/* ────────────────────────────────────────────────────────────────────
 * PCA9685 register map
 * ────────────────────────────────────────────────────────────────────
 *
 * These constants define the register addresses used by the PCA9685
 * PWM controller. Communication happens through the Linux I²C SMBus
 * helper functions.
 */

#define PCA9685_REG_MODE1	0x00   /* mode control register */
#define PCA9685_REG_PRESCALE	0xFE   /* PWM frequency prescaler */
#define PCA9685_REG_LED0_ON_L	0x06   /* start of channel register block */

#define PCA9685_CHAN_OFF(ch)	(4 * (ch))
/*
 * Each PWM channel uses 4 registers:
 *
 *   LEDn_ON_L
 *   LEDn_ON_H
 *   LEDn_OFF_L
 *   LEDn_OFF_H
 *
 * So channel N starts at:
 *   base + 4*N
 */

#define MODE1_SLEEP		0x10   /* put chip into low power state */
#define MODE1_AI		0x20   /* auto-increment register pointer */
#define MODE1_RESTART		0x80   /* restart PWM engine */

#define PCA9685_I2C_ADDR	0x40   /* default I²C address */


/* ────────────────────────────────────────────────────────────────────
 * PWM calculation constants
 * ────────────────────────────────────────────────────────────────────
 *
 * Servos operate at ~50 Hz (20 ms period).
 *
 * PCA9685 resolution:
 *   4096 ticks per PWM period
 *
 * Servo pulse ranges:
 *   1.0 ms → minimum angle
 *   2.0 ms → maximum angle
 *
 * These constants convert angles to PCA9685 tick values.
 */

#define SERVO_TICKS_MIN		205	/* ≈ 1 ms pulse width */
#define SERVO_TICKS_MAX		410	/* ≈ 2 ms pulse width */

#define SERVO_TICKS_RANGE	(SERVO_TICKS_MAX - SERVO_TICKS_MIN)

/*
 * Prescale value for generating a 50 Hz PWM signal.
 *
 * Formula from PCA9685 datasheet:
 *
 *   prescale = round(25MHz / (4096 * freq)) - 1
 *
 * For 50 Hz → 0x79
 */

#define PCA9685_PRESCALE_50HZ	0x79


/* ────────────────────────────────────────────────────────────────────
 * Private driver context
 * ────────────────────────────────────────────────────────────────────
 *
 * This structure stores all runtime state for the servo subsystem.
 */

struct echo_servo_ctx {
	struct i2c_adapter *adapter; /* Linux I²C bus interface */
	struct i2c_client  *client;  /* PCA9685 device instance */

	bool sim_mode;               /* skip hardware access if true */

	u16 pos[ECHO_NUM_SERVOS];    /* last known servo angles */

	struct mutex lock;           /* protects shared state */
};


/* ────────────────────────────────────────────────────────────────────
 * Low-level I²C helper
 * ────────────────────────────────────────────────────────────────────
 *
 * Writes a single byte to a PCA9685 register using the SMBus helper.
 */

static int pca9685_write_byte(struct i2c_client *c, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(c, reg, val);
}


/* ────────────────────────────────────────────────────────────────────
 * Convert angle → PWM ticks
 * ────────────────────────────────────────────────────────────────────
 *
 * Converts a servo angle (0–180 degrees) into the corresponding
 * PCA9685 PWM tick value.
 *
 * Floating point is not allowed in kernel code, so this calculation
 * uses integer arithmetic.
 */

static u16 angle_to_pwm(u16 angle)
{
	return SERVO_TICKS_MIN + (angle * SERVO_TICKS_RANGE) / 180;
}


/* ────────────────────────────────────────────────────────────────────
 * Set PWM output for a single channel
 * ────────────────────────────────────────────────────────────────────
 *
 * Programs the ON and OFF tick values for a specific PCA9685 channel.
 *
 * For servo control we typically:
 *    ON  = 0
 *    OFF = pulse width ticks
 */

static int pca9685_set_pwm(struct i2c_client *c, u8 channel,
			    u16 on, u16 off)
{
	u8 base = PCA9685_REG_LED0_ON_L + PCA9685_CHAN_OFF(channel);
	int ret;

	ret = pca9685_write_byte(c, base,     on & 0xFF);
	if (ret < 0) return ret;

	ret = pca9685_write_byte(c, base + 1, (on >> 8) & 0x0F);
	if (ret < 0) return ret;

	ret = pca9685_write_byte(c, base + 2, off & 0xFF);
	if (ret < 0) return ret;

	ret = pca9685_write_byte(c, base + 3, (off >> 8) & 0x0F);

	return ret;
}


/* ────────────────────────────────────────────────────────────────────
 * PCA9685 hardware initialisation
 * ────────────────────────────────────────────────────────────────────
 *
 * Configures the PWM controller for 50 Hz servo operation.
 */

static int pca9685_init_hw(struct i2c_client *c)
{
	int ret;
	u8 mode1;

	/* Put the device into sleep mode before changing prescale */
	ret = pca9685_write_byte(c, PCA9685_REG_MODE1, MODE1_SLEEP);
	if (ret < 0) return ret;

	/* Set PWM frequency */
	ret = pca9685_write_byte(c, PCA9685_REG_PRESCALE,
				 PCA9685_PRESCALE_50HZ);
	if (ret < 0) return ret;

	/* Enable auto-increment addressing */
	mode1 = MODE1_AI;
	ret = pca9685_write_byte(c, PCA9685_REG_MODE1, mode1);
	if (ret < 0) return ret;

	usleep_range(500, 1000);

	/* Restart PWM engine */
	mode1 = MODE1_RESTART | MODE1_AI;
	return pca9685_write_byte(c, PCA9685_REG_MODE1, mode1);
}


/* ────────────────────────────────────────────────────────────────────
 * Public API — create subsystem
 * ────────────────────────────────────────────────────────────────────
 *
 * Allocates and initialises the servo subsystem.
 *
 * If sim_mode is enabled, hardware initialisation is skipped.
 */

struct echo_servo_ctx *echo_servo_create(bool sim_mode)
{
	struct echo_servo_ctx *ctx;
	struct i2c_adapter *adapter;
	struct i2c_client *client;

	struct i2c_board_info board_info = {
		I2C_BOARD_INFO("pca9685", PCA9685_I2C_ADDR),
	};

	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	mutex_init(&ctx->lock);

	ctx->sim_mode = sim_mode;

	/* Initialise servo positions to centre */
	ctx->pos[ECHO_SERVO_PAN]   = ECHO_SERVO_CENTER;
	ctx->pos[ECHO_SERVO_TILT]  = ECHO_SERVO_CENTER;
	ctx->pos[ECHO_SERVO_TILT2] = ECHO_SERVO_CENTER;

	if (sim_mode) {
		pr_info("echo: servo: simulation mode\n");
		return ctx;
	}

	/* Acquire I²C adapter for bus 1 */
	adapter = i2c_get_adapter(1);
	if (!adapter) {
		pr_err("echo: servo: I2C adapter 1 not found\n");
		kfree(ctx);
		return ERR_PTR(-ENODEV);
	}

	/* Create a new I²C client device for PCA9685 */
	client = i2c_new_client_device(adapter, &board_info);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		pr_err("echo: servo: I2C client failed (%d)\n", ret);
		i2c_put_adapter(adapter);
		kfree(ctx);
		return ERR_PTR(ret);
	}

	ctx->adapter = adapter;
	ctx->client  = client;

	/* Initialise PWM controller */
	ret = pca9685_init_hw(client);
	if (ret < 0) {
		pr_err("echo: servo: PCA9685 init failed (%d)\n", ret);
		i2c_unregister_device(client);
		i2c_put_adapter(adapter);
		kfree(ctx);
		return ERR_PTR(ret);
	}

	/* Centre all servos at startup */
	pca9685_set_pwm(client, ECHO_SERVO_PAN,  0,
			angle_to_pwm(ECHO_SERVO_CENTER));

	pca9685_set_pwm(client, ECHO_SERVO_TILT, 0,
			angle_to_pwm(ECHO_SERVO_CENTER));

	pca9685_set_pwm(client, ECHO_SERVO_TILT2, 0,
			angle_to_pwm(ECHO_SERVO_CENTER));

	pr_info("echo: servo: PCA9685 initialised on I2C bus 1\n");

	return ctx;
}


/* ────────────────────────────────────────────────────────────────────
 * Destroy subsystem
 * ────────────────────────────────────────────────────────────────────
 *
 * Shuts down hardware and frees resources.
 */

void echo_servo_destroy(struct echo_servo_ctx *ctx)
{
	if (!ctx)
		return;

	if (!ctx->sim_mode) {

		/* Stop PWM outputs */
		if (ctx->client) {
			pca9685_set_pwm(ctx->client, ECHO_SERVO_PAN,   0, 0);
			pca9685_set_pwm(ctx->client, ECHO_SERVO_TILT,  0, 0);
			pca9685_set_pwm(ctx->client, ECHO_SERVO_TILT2, 0, 0);

			i2c_unregister_device(ctx->client);
		}

		if (ctx->adapter)
			i2c_put_adapter(ctx->adapter);
	}

	kfree(ctx);
}


/* ────────────────────────────────────────────────────────────────────
 * Set servo angle
 * ────────────────────────────────────────────────────────────────────
 *
 * Updates a servo position and sends the corresponding PWM command
 * to the PCA9685.
 */

int echo_servo_set_angle(struct echo_servo_ctx *ctx, u8 servo_id, u16 angle)
{
	u16 pwm_ticks;
	int ret = 0;

	if (servo_id >= ECHO_NUM_SERVOS)
		return -EINVAL;

	if (angle > ECHO_SERVO_MAX)
		angle = ECHO_SERVO_MAX;

	mutex_lock(&ctx->lock);

	ctx->pos[servo_id] = angle;

	/* Tilt2 mirrors the base — keep them synchronised */
	if (servo_id == ECHO_SERVO_PAN)
		ctx->pos[ECHO_SERVO_TILT2] = angle;

	if (ctx->sim_mode) {
		pr_info("echo: servo[%d] = %u (sim)\n", servo_id, angle);

		if (servo_id == ECHO_SERVO_PAN)
			pr_info("echo: servo[%d] = %u (sim, mirror)\n",
				ECHO_SERVO_TILT2, angle);

		mutex_unlock(&ctx->lock);
		return 0;
	}

	pwm_ticks = angle_to_pwm(angle);

	ret = pca9685_set_pwm(ctx->client, servo_id, 0, pwm_ticks);

	/* Mirror tilt2 with pan */
	if (servo_id == ECHO_SERVO_PAN && ret == 0)
		ret = pca9685_set_pwm(ctx->client,
				      ECHO_SERVO_TILT2,
				      0,
				      pwm_ticks);

	mutex_unlock(&ctx->lock);

	return ret;
}


/* ────────────────────────────────────────────────────────────────────
 * Get servo angle
 * ────────────────────────────────────────────────────────────────────
 *
 * Returns the last known position of a servo.
 */

u16 echo_servo_get_angle(struct echo_servo_ctx *ctx, u8 servo_id)
{
	u16 angle;

	if (servo_id >= ECHO_NUM_SERVOS)
		return ECHO_SERVO_CENTER;

	mutex_lock(&ctx->lock);
	angle = ctx->pos[servo_id];
	mutex_unlock(&ctx->lock);

	return angle;
}