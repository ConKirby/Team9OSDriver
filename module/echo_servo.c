// SPDX-License-Identifier: GPL-2.0
/*
 * echo_servo.c — I2C + PCA9685 hardware abstraction (leaf subsystem)
 *
 * Drives two servos (pan/tilt) via a PCA9685 16-channel PWM controller
 * on I2C bus 1.  In simulation mode all I2C traffic is skipped.
 *
 * Dependencies: NONE — this is a leaf subsystem.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "echo_types.h"
#include "echo_servo.h"

/* ── PCA9685 register map ──────────────────────────────────────────── */
#define PCA9685_REG_MODE1	0x00
#define PCA9685_REG_PRESCALE	0xFE
#define PCA9685_REG_LED0_ON_L	0x06
#define PCA9685_CHAN_OFF(ch)	(4 * (ch))

#define MODE1_SLEEP		0x10
#define MODE1_AI		0x20
#define MODE1_RESTART		0x80

#define PCA9685_I2C_ADDR	0x40

/* ── PWM calculation (50 Hz, 4096 ticks per 20 ms period) ─────────── */
#define SERVO_TICKS_MIN		205	/* 1 ms / 20 ms * 4096 */
#define SERVO_TICKS_MAX		410	/* 2 ms / 20 ms * 4096 */
#define SERVO_TICKS_RANGE	(SERVO_TICKS_MAX - SERVO_TICKS_MIN)
#define PCA9685_PRESCALE_50HZ	0x79	/* round(25 MHz / (4096 * 50)) - 1 */

/* ── Private context ───────────────────────────────────────────────── */
struct echo_servo_ctx {
	struct i2c_adapter *adapter;
	struct i2c_client  *client;
	bool sim_mode;
	u16 pos[ECHO_NUM_SERVOS];
	struct mutex lock;
};

/* ── Low-level I2C helpers ─────────────────────────────────────────── */

static int pca9685_write_byte(struct i2c_client *c, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(c, reg, val);
}

/* ── Angle -> PWM ticks (integer-only, no floating point) ──────────── */

static u16 angle_to_pwm(u16 angle)
{
	return SERVO_TICKS_MIN + (angle * SERVO_TICKS_RANGE) / 180;
}

/* ── Set a single PCA9685 PWM channel ──────────────────────────────── */

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

/* ── PCA9685 hardware initialisation ───────────────────────────────── */

static int pca9685_init_hw(struct i2c_client *c)
{
	int ret;
	u8 mode1;

	ret = pca9685_write_byte(c, PCA9685_REG_MODE1, MODE1_SLEEP);
	if (ret < 0) return ret;

	ret = pca9685_write_byte(c, PCA9685_REG_PRESCALE,
				 PCA9685_PRESCALE_50HZ);
	if (ret < 0) return ret;

	mode1 = MODE1_AI;
	ret = pca9685_write_byte(c, PCA9685_REG_MODE1, mode1);
	if (ret < 0) return ret;

	usleep_range(500, 1000);

	mode1 = MODE1_RESTART | MODE1_AI;
	return pca9685_write_byte(c, PCA9685_REG_MODE1, mode1);
}

/* ── Public API ────────────────────────────────────────────────────── */

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
	ctx->pos[ECHO_SERVO_PAN]  = ECHO_SERVO_CENTER;
	ctx->pos[ECHO_SERVO_TILT] = ECHO_SERVO_CENTER;

	if (sim_mode) {
		pr_info("echo: servo: simulation mode\n");
		return ctx;
	}

	/* Real hardware path */
	adapter = i2c_get_adapter(1);
	if (!adapter) {
		pr_err("echo: servo: I2C adapter 1 not found\n");
		kfree(ctx);
		return ERR_PTR(-ENODEV);
	}

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

	ret = pca9685_init_hw(client);
	if (ret < 0) {
		pr_err("echo: servo: PCA9685 init failed (%d)\n", ret);
		i2c_unregister_device(client);
		i2c_put_adapter(adapter);
		kfree(ctx);
		return ERR_PTR(ret);
	}

	/* Centre both servos */
	pca9685_set_pwm(client, ECHO_SERVO_PAN,  0,
			angle_to_pwm(ECHO_SERVO_CENTER));
	pca9685_set_pwm(client, ECHO_SERVO_TILT, 0,
			angle_to_pwm(ECHO_SERVO_CENTER));

	pr_info("echo: servo: PCA9685 initialised on I2C bus 1\n");
	return ctx;
}

void echo_servo_destroy(struct echo_servo_ctx *ctx)
{
	if (!ctx)
		return;

	if (!ctx->sim_mode) {
		if (ctx->client) {
			pca9685_set_pwm(ctx->client, ECHO_SERVO_PAN,  0, 0);
			pca9685_set_pwm(ctx->client, ECHO_SERVO_TILT, 0, 0);
			i2c_unregister_device(ctx->client);
		}
		if (ctx->adapter)
			i2c_put_adapter(ctx->adapter);
	}

	kfree(ctx);
}

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

	if (ctx->sim_mode) {
		pr_info("echo: servo[%d] = %u (sim)\n", servo_id, angle);
		mutex_unlock(&ctx->lock);
		return 0;
	}

	pwm_ticks = angle_to_pwm(angle);
	ret = pca9685_set_pwm(ctx->client, servo_id, 0, pwm_ticks);
	mutex_unlock(&ctx->lock);
	return ret;
}

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
