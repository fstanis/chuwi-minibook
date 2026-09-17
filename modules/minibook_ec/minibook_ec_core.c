// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Chuwi MiniBook EC platform driver (ITE IT5570E)
 *
 * Exposes:
 *   - Touchpad toggle and keyboard/tablet mode (sysfs)
 *   - EC firmware version and BIOS menu unlock (sysfs)
 *   - ACMK accelerometer methods as binary attributes (gmtr, crs)
 *   - SoC temperature as a thermal zone (EC register 0x70)
 *   - Fan RPM and PWM duty cycle as hwmon (IT5570E PWM module, I2EC)
 */

#include <linux/acpi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include "minibook_ec.h"

static int minibook_ec_probe(struct platform_device *pdev)
{
	struct minibook_ec *ec;
	u8 tptl, kbcd;
	int ret;

	ec = devm_kzalloc(&pdev->dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->pdev = pdev;
	mutex_init(&ec->i2ec_lock);
	platform_set_drvdata(pdev, ec);

	ret = ec_read(EC_REG_TPTL, &tptl);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read EC register 0x%02x\n",
				     EC_REG_TPTL);

	ret = ec_read(EC_REG_KBCD, &kbcd);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read EC register 0x%02x\n",
				     EC_REG_KBCD);

	ret = minibook_ec_register_sysfs(ec);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register sysfs attrs\n");

	ret = minibook_ec_register_acpi(ec);
	if (ret < 0)
		dev_warn(&pdev->dev, "ACMK attributes not available (%d)\n",
			 ret);

	ret = minibook_ec_i2ec_init(ec);
	if (ret < 0)
		dev_warn(&pdev->dev, "I2EC unavailable (%d)\n", ret);

	ret = minibook_ec_register_thermal(ec);
	if (ret < 0)
		dev_warn(&pdev->dev, "thermal zones not available (%d)\n", ret);

	ret = minibook_ec_register_fan(ec);
	if (ret < 0)
		dev_warn(&pdev->dev, "fan hwmon not available (%d)\n", ret);

	ret = minibook_ec_register_kbd_backlight(ec);
	if (ret < 0)
		dev_warn(&pdev->dev, "kbd backlight LED not available (%d)\n",
			 ret);

	dev_info(&pdev->dev,
		 "touchpad %s, keyboard %s, SoC %s, charger %s, fan %s, kbd_bl %s\n",
		 tptl != TPTL_OFF ? "enabled" : "disabled",
		 kbcd == KBCD_LAPTOP ? "enabled" : "disabled",
		 ec->soc_tz ? "registered" : "unavailable",
		 ec->charger_tz ? "registered" : "unavailable",
		 ec->hwmon_dev ? "registered" : "unavailable",
		 ec->kbd_bl ? "registered" : "unavailable");

	return 0;
}

static void minibook_ec_remove(struct platform_device *pdev)
{
	struct minibook_ec *ec = platform_get_drvdata(pdev);

	minibook_ec_unregister_thermal(ec);
	minibook_ec_unregister_fan(ec);
	minibook_ec_i2ec_cleanup(ec);
}

static struct platform_driver minibook_ec_driver = {
	.probe	= minibook_ec_probe,
	.remove	= minibook_ec_remove,
	.driver	= {
		.name = DRIVER_NAME,
	},
};

static struct platform_device *minibook_ec_pdev;

static int __init minibook_ec_init(void)
{
	int ret;

	ret = platform_driver_register(&minibook_ec_driver);
	if (ret)
		return ret;

	minibook_ec_pdev = platform_device_register_simple(DRIVER_NAME,
							   -1, NULL, 0);
	if (IS_ERR(minibook_ec_pdev)) {
		platform_driver_unregister(&minibook_ec_driver);
		return PTR_ERR(minibook_ec_pdev);
	}

	return 0;
}

static void __exit minibook_ec_exit(void)
{
	platform_device_unregister(minibook_ec_pdev);
	platform_driver_unregister(&minibook_ec_driver);
}

module_init(minibook_ec_init);
module_exit(minibook_ec_exit);

MODULE_AUTHOR("Filip Stanis");
MODULE_DESCRIPTION("Chuwi MiniBook EC platform driver");
MODULE_LICENSE("GPL");
