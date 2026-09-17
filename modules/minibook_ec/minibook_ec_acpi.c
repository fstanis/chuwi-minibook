// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Binary sysfs attributes for the ACMK accelerometer ACPI methods:
 *   - gmtr: raw GMTR calibration blob (calibration matrices, thresholds)
 *   - crs:  raw _CRS resource buffer, used to derive the sensor order
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "minibook_ec.h"

#define ACMK_GMTR_PATH	"\\_SB.ACMK.GMTR"
#define ACMK_CRS_PATH	"\\_SB.ACMK._CRS"

static ssize_t eval_acpi_buffer(const char *path, char *buf, loff_t off,
				size_t count)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	ssize_t ret;

	if (ACPI_FAILURE(acpi_evaluate_object(NULL, (acpi_string)path, NULL,
					      &out)))
		return -EIO;

	obj = out.pointer;
	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -EIO;
	} else if (off >= obj->buffer.length) {
		ret = 0;
	} else {
		ret = min_t(size_t, count, obj->buffer.length - off);
		memcpy(buf, obj->buffer.pointer + off, ret);
	}

	kfree(obj);
	return ret;
}

static ssize_t gmtr_read(struct file *filp, struct kobject *kobj,
			 const struct bin_attribute *attr, char *buf,
			 loff_t off, size_t count)
{
	return eval_acpi_buffer(ACMK_GMTR_PATH, buf, off, count);
}

static ssize_t crs_read(struct file *filp, struct kobject *kobj,
			const struct bin_attribute *attr, char *buf,
			loff_t off, size_t count)
{
	return eval_acpi_buffer(ACMK_CRS_PATH, buf, off, count);
}

static BIN_ATTR_RO(gmtr, 0);
static BIN_ATTR_RO(crs, 0);

static const struct bin_attribute *const minibook_ec_acpi_bin_attrs[] = {
	&bin_attr_gmtr,
	&bin_attr_crs,
	NULL,
};

static const struct attribute_group minibook_ec_acpi_group = {
	.bin_attrs = minibook_ec_acpi_bin_attrs,
};

int minibook_ec_register_acpi(struct minibook_ec *ec)
{
	return devm_device_add_group(&ec->pdev->dev, &minibook_ec_acpi_group);
}
