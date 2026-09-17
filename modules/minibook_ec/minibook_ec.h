/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MINIBOOK_EC_H
#define MINIBOOK_EC_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/types.h>

#define DRIVER_NAME	"minibook_ec"

/* ACPI EC register window (0x00-0xFF, accessed via ec_read/ec_write) */
#define EC_REG_ECMV	0x00
#define EC_REG_ECSV	0x01
#define EC_REG_KBVS	0x02
#define EC_REG_ECTV	0x03
#define EC_REG_TPTL	0x0C
#define EC_REG_KBCD	0x0D
#define EC_REG_SOC_TEMP	0x70
#define EC_REG_UNLOCK	0xF0

#define TPTL_ON		0x11
#define TPTL_OFF	0x22

#define KBCD_LAPTOP	0x00
#define KBCD_SLATE	0x03

#define UNLOCK_MAGIC	0xAA

#define LTSM_PATH	"\\_SB.ACMK.LTSM"

struct kbd_backlight;

struct minibook_ec {
	struct platform_device *pdev;
	struct thermal_zone_device *soc_tz;
	struct thermal_zone_device *charger_tz;
	struct device *hwmon_dev;
	struct kbd_backlight *kbd_bl;
	struct mutex i2ec_lock;
	bool pnpcfg_available;
};

int minibook_ec_register_sysfs(struct minibook_ec *ec);

int minibook_ec_register_acpi(struct minibook_ec *ec);

int minibook_ec_register_thermal(struct minibook_ec *ec);
void minibook_ec_unregister_thermal(struct minibook_ec *ec);

int minibook_ec_register_fan(struct minibook_ec *ec);
void minibook_ec_unregister_fan(struct minibook_ec *ec);

int minibook_ec_register_kbd_backlight(struct minibook_ec *ec);

int minibook_ec_i2ec_init(struct minibook_ec *ec);
void minibook_ec_i2ec_cleanup(struct minibook_ec *ec);
u8 minibook_ec_i2ec_read(struct minibook_ec *ec, u16 addr);
void minibook_ec_i2ec_write(struct minibook_ec *ec, u16 addr, u8 val);

#endif
