// SPDX-License-Identifier: GPL-2.0
/*
 * echo_servo.c — I2C + PCA9685 hardware abstraction for Echo Robot servos.
 *
 * Drives two servos (pan/tilt) via a PCA9685 16-channel PWM controller
 * connected on I2C bus 1.  When the driver is loaded in simulation mode
 * all I2C traffic is skipped and angles are only stored in software.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/printk.h>
#include <linux/types.h>

#include "echo_main.h"
#include "echo_servo.h"

/* ------------------------------------------------------------------ */
/* PCA9685 register map                                                */
/* ------------------------------------------------------------------ */
#define PCA9685_REG_MODE1       0x00
#define PCA9685_REG_MODE2       0x01
#define PCA9685_REG_PRESCALE    0xFE

#define PCA9685_REG_LED0_ON_L   0x06
#define PCA9685_REG_LED0_ON_H   0x07
#define PCA9685_REG_LED0_OFF_L  0x08
#define PCA9685_REG_LED0_OFF_H  0x09

/* Per-channel register offset: base + 4 * channel */
#define PCA9685_CHAN_OFF(ch)     (4 * (ch))

/* MODE1 bits */
#define MODE1_SLEEP             0x10
#define MODE1_AI                0x20    /* auto-increment */
#define MODE1_RESTART           0x80

/* I2C address */
#define PCA9685_I2C_ADDR        0x40

/* ------------------------------------------------------------------ */
/* PWM calculation constants (50 Hz, 20 ms period, 4096 ticks)         */
/* ------------------------------------------------------------------ */
#define PCA9685_TICK_TOTAL      4096
#define PCA9685_FREQ_HZ         50

/* Servo pulse widths expressed in ticks:
 *   0°   -> 1 ms  -> 205 ticks   (1 ms / 20 ms * 4096)
 *   180° -> 2 ms  -> 410 ticks   (2 ms / 20 ms * 4096)
 */
#define SERVO_TICKS_MIN         205
#define SERVO_TICKS_MAX         410
#define SERVO_TICKS_RANGE       (SERVO_TICKS_MAX - SERVO_TICKS_MIN)  /* 205 */

/*
 * Prescale value for 50 Hz:
 *   prescale = round(25 MHz / (4096 * 50)) - 1 = 121 (0x79)
 */
#define PCA9685_PRESCALE_50HZ   0x79

/* ------------------------------------------------------------------ */
/* Low-level I2C helpers                                               */
/* ------------------------------------------------------------------ */

static int pca9685_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static int pca9685_read_byte(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

/* ------------------------------------------------------------------ */
/* Angle -> PWM tick conversion                                        */
/* ------------------------------------------------------------------ */

static u16 angle_to_pwm(u16 angle)
{
	/* 205 + (angle * 205) / 180 */
	return SERVO_TICKS_MIN + (angle * SERVO_TICKS_RANGE) / 180;
}

/* ------------------------------------------------------------------ */
/* PCA9685 hardware initialisation                                     */
/* ------------------------------------------------------------------ */

static int pca9685_init_hw(struct i2c_client *client)
{
	int ret;
	u8 mode1;

	/* 1. Put the chip to sleep so we can change the prescaler. */
	ret = pca9685_write_byte(client, PCA9685_REG_MODE1, MODE1_SLEEP);
	if (ret < 0)
		return ret;

	/* 2. Set prescaler for 50 Hz. */
	ret = pca9685_write_byte(client, PCA9685_REG_PRESCALE,
				 PCA9685_PRESCALE_50HZ);
	if (ret < 0)
		return ret;

	/* 3. Wake up: clear sleep, enable auto-increment. */
	mode1 = MODE1_AI;
	ret = pca9685_write_byte(client, PCA9685_REG_MODE1, mode1);
	if (ret < 0)
		return ret;

	/* 4. Wait for oscillator to stabilise. */
	usleep_range(500, 1000);

	/* 5. Set the restart bit (keeps auto-increment enabled). */
	mode1 = MODE1_RESTART | MODE1_AI;
	ret = pca9685_write_byte(client, PCA9685_REG_MODE1, mode1);
	if (ret < 0)
		return ret;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Set a single PWM channel                                            */
/* ------------------------------------------------------------------ */

static int pca9685_set_pwm(struct i2c_client *client, u8 channel,
			    u16 on, u16 off)
{
	u8 base = PCA9685_REG_LED0_ON_L + PCA9685_CHAN_OFF(channel);
	int ret;

	ret = pca9685_write_byte(client, base,     on & 0xFF);
	if (ret < 0)
		return ret;

	ret = pca9685_write_byte(client, base + 1, (on >> 8) & 0x0F);
	if (ret < 0)
		return ret;

	ret = pca9685_write_byte(client, base + 2, off & 0xFF);
	if (ret < 0)
		return ret;

	ret = pca9685_write_byte(client, base + 3, (off >> 8) & 0x0F);
	if (ret < 0)
		return ret;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int echo_servo_init(struct echo_device *dev)
{
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	struct i2c_board_info board_info = {
		I2C_BOARD_INFO("pca9685", PCA9685_I2C_ADDR),
	};
	int ret;

	/* Simulation mode — no hardware access. */
	if (dev->sim_mode) {
		pr_info("Servo: simulation mode\n");
		dev->servo_pos[ECHO_SERVO_PAN]  = ECHO_SERVO_CENTER;
		dev->servo_pos[ECHO_SERVO_TILT] = ECHO_SERVO_CENTER;
		return 0;
	}

	/* Get I2C adapter (bus 1). */
	adapter = i2c_get_adapter(1);
	if (!adapter) {
		pr_err("Servo: I2C adapter 1 not found\n");
		return -ENODEV;
	}

	/* Register a new I2C client for the PCA9685. */
	client = i2c_new_client_device(adapter, &board_info);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		pr_err("Servo: failed to create I2C client (%d)\n", ret);
		i2c_put_adapter(adapter);
		return ret;
	}

	dev->i2c_adapter = adapter;
	dev->i2c_client  = client;

	/* Initialise the PCA9685 hardware. */
	ret = pca9685_init_hw(client);
	if (ret < 0) {
		pr_err("Servo: PCA9685 init failed (%d)\n", ret);
		i2c_unregister_device(client);
		i2c_put_adapter(adapter);
		dev->i2c_client  = NULL;
		dev->i2c_adapter = NULL;
		return ret;
	}

	/* Centre both servos at 90°. */
	dev->servo_pos[ECHO_SERVO_PAN]  = ECHO_SERVO_CENTER;
	dev->servo_pos[ECHO_SERVO_TILT] = ECHO_SERVO_CENTER;

	pca9685_set_pwm(client, ECHO_SERVO_PAN,  0,
			 angle_to_pwm(ECHO_SERVO_CENTER));
	pca9685_set_pwm(client, ECHO_SERVO_TILT, 0,
			 angle_to_pwm(ECHO_SERVO_CENTER));

	pr_info("Servo: PCA9685 initialized on I2C bus 1\n");
	return 0;
}

void echo_servo_cleanup(struct echo_device *dev)
{
	if (dev->sim_mode)
		return;

	if (dev->i2c_client) {
		/* Turn both channels fully off. */
		pca9685_set_pwm(dev->i2c_client, ECHO_SERVO_PAN,  0, 0);
		pca9685_set_pwm(dev->i2c_client, ECHO_SERVO_TILT, 0, 0);
		i2c_unregister_device(dev->i2c_client);
		dev->i2c_client = NULL;
	}

	if (dev->i2c_adapter) {
		i2c_put_adapter(dev->i2c_adapter);
		dev->i2c_adapter = NULL;
	}
}

int echo_servo_set_angle(struct echo_device *dev, u8 servo_id, u16 angle)
{
	u16 pwm_ticks;
	int ret = 0;

	if (servo_id >= ECHO_NUM_SERVOS)
		return -EINVAL;

	/* Clamp into the valid range */
	if (angle > ECHO_SERVO_MAX)
		angle = ECHO_SERVO_MAX;

	mutex_lock(&dev->servo_lock);

	dev->servo_pos[servo_id] = angle;

	if (dev->sim_mode) {
		pr_info("Servo[%d] = %u° (sim)\n", servo_id, angle);
		mutex_unlock(&dev->servo_lock);
		return 0;
	}

	pwm_ticks = angle_to_pwm(angle);
	ret = pca9685_set_pwm(dev->i2c_client, servo_id, 0, pwm_ticks);

	mutex_unlock(&dev->servo_lock);
	return ret;
}

u16 echo_servo_get_angle(struct echo_device *dev, u8 servo_id)
{
	u16 angle;

	if (servo_id >= ECHO_NUM_SERVOS)
		return ECHO_SERVO_CENTER;

	mutex_lock(&dev->servo_lock);
	angle = dev->servo_pos[servo_id];
	mutex_unlock(&dev->servo_lock);

	return angle;
}
