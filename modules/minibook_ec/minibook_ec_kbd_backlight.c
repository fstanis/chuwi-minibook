// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Keyboard backlight exposed as a Linux LED class device with two
 * orthogonal sysfs controls:
 *
 *   brightness      (LED class, 0-255) — PWM duty on IT5570E DCR1
 *   auto_off_timer  (0 or 1)           — 30-tick auto-off timer at 0x0411
 *
 * Setting brightness writes the PWM duty at DCR1 (0x1803) and the firmware
 * setpoint at 0x03E2. If auto_off_timer is currently suspended, the driver
 * also opens the GPIO gate (0x1611 bit 6) on brightness > 0 and closes it
 * on brightness == 0, because the EC firmware only manages the gate itself
 * in response to keyboard activity — which can be arbitrarily long away.
 *
 * If auto_off_timer is active, the firmware's activity-reset path drives
 * the gate based on setpoint: we do not touch the gate from the driver,
 * because we'd be racing the EC.
 *
 * auto_off_timer == 1 → timer at 0x03E1 decrements each tick (~1 Hz); on
 * expiry the firmware closes the gate. Keyboard activity re-arms it.
 * auto_off_timer == 0 → writes 0x0411 = 0xEE; firmware skips the decrement,
 * matching BIOS "Keyboard Backlight Mode = Always on".
 *
 * The EC firmware silently mutates the LED on its own (BIOS-defined Fn-key
 * toggles brightness, auto-off timer closes the gate). It does not raise an
 * SCI / ACPI Notify, so the host learns about EC-initiated changes only by
 * reading the PWM. The optional poll_hw_changed module parameter enables a
 * 250 ms delayed_work that surfaces deltas via LED_BRIGHT_HW_CHANGED so
 * UPower / GNOME / KDE can pick them up; off by default.
 */

#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/moduleparam.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "minibook_ec.h"

static bool poll_hw_changed;
module_param(poll_hw_changed, bool, 0444);
MODULE_PARM_DESC(poll_hw_changed,
		 "Opt in to polling the EC and surfacing EC-initiated brightness changes via the LED-class brightness_hw_changed attribute (default: off)");

#define IT5570E_KBD_BL_DCR		0x1803
#define IT5570E_KBD_BL_GPIO_GATE	0x1611
#define KBD_BL_GPIO_GATE_OFF		0x40

#define EC_KBD_BL_SETPOINT		0x03E2
#define EC_KBD_BL_TIMER_SUSPEND		0x0411
#define KBD_BL_TIMER_SUSPEND_MAGIC	0xEE

#define KBD_BL_MAX_BRIGHTNESS		0xFF
#define KBD_BL_LED_NAME			"minibook::kbd_backlight"
#define KBD_BL_POLL_INTERVAL		(HZ / 4)

struct kbd_backlight {
	struct led_classdev cdev;
	struct minibook_ec *ec;
	struct delayed_work poll_work;
	u8 last_observed;
};

static struct kbd_backlight *dev_to_kbl(struct device *dev)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);

	return container_of(cdev, struct kbd_backlight, cdev);
}

static struct minibook_ec *cdev_to_ec(struct led_classdev *cdev)
{
	struct kbd_backlight *kbl;

	kbl = container_of(cdev, struct kbd_backlight, cdev);
	return kbl->ec;
}

static bool kbd_bl_output_gated(struct minibook_ec *ec)
{
	u8 gpio = minibook_ec_i2ec_read(ec, IT5570E_KBD_BL_GPIO_GATE);

	return gpio & KBD_BL_GPIO_GATE_OFF;
}

static void kbd_bl_set_output(struct minibook_ec *ec, bool enable)
{
	u8 gpio = minibook_ec_i2ec_read(ec, IT5570E_KBD_BL_GPIO_GATE);

	if (enable)
		gpio &= ~KBD_BL_GPIO_GATE_OFF;
	else
		gpio |= KBD_BL_GPIO_GATE_OFF;
	minibook_ec_i2ec_write(ec, IT5570E_KBD_BL_GPIO_GATE, gpio);
}

static void kbd_bl_set_duty(struct minibook_ec *ec, u8 duty)
{
	minibook_ec_i2ec_write(ec, IT5570E_KBD_BL_DCR, duty);
	minibook_ec_i2ec_write(ec, EC_KBD_BL_SETPOINT, duty);
}

static bool kbd_bl_timer_suspended(struct minibook_ec *ec)
{
	u8 flag = minibook_ec_i2ec_read(ec, EC_KBD_BL_TIMER_SUSPEND);

	return flag == KBD_BL_TIMER_SUSPEND_MAGIC;
}

static void kbd_bl_set_auto_off(struct minibook_ec *ec, bool enable)
{
	u8 val = 0;
	if (!enable)
		val = KBD_BL_TIMER_SUSPEND_MAGIC;

	minibook_ec_i2ec_write(ec, EC_KBD_BL_TIMER_SUSPEND, val);
}

static int kbd_bl_brightness_set(struct led_classdev *cdev,
				 enum led_brightness value)
{
	struct kbd_backlight *kbl =
		container_of(cdev, struct kbd_backlight, cdev);

	kbd_bl_set_duty(kbl->ec, value);
	if (kbd_bl_timer_suspended(kbl->ec))
		kbd_bl_set_output(kbl->ec, value != 0);
	kbl->last_observed = value;
	return 0;
}

static enum led_brightness kbd_bl_brightness_get(struct led_classdev *cdev)
{
	struct minibook_ec *ec = cdev_to_ec(cdev);

	if (kbd_bl_output_gated(ec))
		return LED_OFF;
	return minibook_ec_i2ec_read(ec, IT5570E_KBD_BL_DCR);
}

static void kbd_bl_poll_work(struct work_struct *work)
{
	struct kbd_backlight *kbl = container_of(to_delayed_work(work),
						 struct kbd_backlight,
						 poll_work);
	enum led_brightness current_bri = kbd_bl_brightness_get(&kbl->cdev);

	if (current_bri != kbl->last_observed) {
		kbl->last_observed = current_bri;
		led_classdev_notify_brightness_hw_changed(&kbl->cdev,
							  current_bri);
	}

	queue_delayed_work(system_power_efficient_wq, &kbl->poll_work,
			   round_jiffies_relative(KBD_BL_POLL_INTERVAL));
}

static ssize_t auto_off_timer_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct minibook_ec *ec = dev_to_kbl(dev)->ec;

	return sysfs_emit(buf, "%d\n", !kbd_bl_timer_suspended(ec));
}

static ssize_t auto_off_timer_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct minibook_ec *ec = dev_to_kbl(dev)->ec;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	kbd_bl_set_auto_off(ec, enable);
	return count;
}

static DEVICE_ATTR_RW(auto_off_timer);

static struct attribute *kbd_bl_attrs[] = {
	&dev_attr_auto_off_timer.attr,
	NULL,
};

ATTRIBUTE_GROUPS(kbd_bl);

int minibook_ec_register_kbd_backlight(struct minibook_ec *ec)
{
	struct kbd_backlight *kbl;
	int ret;

	if (!ec->pnpcfg_available)
		return -ENODEV;

	kbl = devm_kzalloc(&ec->pdev->dev, sizeof(*kbl), GFP_KERNEL);
	if (!kbl)
		return -ENOMEM;

	kbl->ec = ec;
	kbl->cdev.name = KBD_BL_LED_NAME;
	kbl->cdev.max_brightness = KBD_BL_MAX_BRIGHTNESS;
	kbl->cdev.brightness_set_blocking = kbd_bl_brightness_set;
	kbl->cdev.brightness_get = kbd_bl_brightness_get;
	kbl->cdev.groups = kbd_bl_groups;
	kbl->cdev.flags = LED_CORE_SUSPENDRESUME;
	if (poll_hw_changed)
		kbl->cdev.flags |= LED_BRIGHT_HW_CHANGED;

	ec->kbd_bl = kbl;
	ret = devm_led_classdev_register(&ec->pdev->dev, &kbl->cdev);
	if (ret)
		return ret;

	if (!poll_hw_changed)
		return 0;

	kbl->last_observed = kbd_bl_brightness_get(&kbl->cdev);
	ret = devm_delayed_work_autocancel(&ec->pdev->dev, &kbl->poll_work,
					   kbd_bl_poll_work);
	if (ret)
		return ret;

	queue_delayed_work(system_power_efficient_wq, &kbl->poll_work,
			   round_jiffies_relative(KBD_BL_POLL_INTERVAL));
	return 0;
}
