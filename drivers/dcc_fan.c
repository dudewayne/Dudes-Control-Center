// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc_fan - fan and temperature access for Clevo notebooks (Dudes Control Center)
 *
 * Step 1 (read-only prototype, 1.36.0 in progress): nothing is written to
 * the EC or sent as a "set" command.
 *
 *  - Reads the EC through the kernel's ACPI EC driver (ec_read), which holds
 *    the EC lock: no race with firmware/ACPI accesses, unlike user-space
 *    port I/O on 0x62/0x66, and no ioperm (works under kernel lockdown).
 *  - Registers a hwmon device "dcc_fan": temp1 (CPU, EC 0x07), temp2 (GPU,
 *    EC 0xCD), fan1/fan2 (RPM from EC 0xD0/0xD1, 0xD2/0xD3), pwm1/pwm2
 *    (duty registers 0xCE/0xCF, read-only).
 *  - Exposes the raw results of the Clevo WMI "get" commands GET_FANINFO1
 *    (0x63), GET_FANINFO2 (0x64), GET_FANINFO3 (0x6e) in
 *    /sys/devices/platform/dcc_fan/wmi_faninfo, so their encoding can be
 *    compared with the EC values. These commands take no argument and only
 *    read (per TUXEDO's clevo_interfaces.h).
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>

#define DCC_FAN_NAME "dcc_fan"
#define CLEVO_WMI_METHOD_GUID "ABBC0F6D-8EA1-11D1-00A0-C90629100000"
#define CLEVO_CMD_GET_FANINFO1 0x63
#define CLEVO_CMD_GET_FANINFO2 0x64
#define CLEVO_CMD_GET_FANINFO3 0x6e

#define EC_REG_CPU_TEMP     0x07
#define EC_REG_GPU_TEMP     0xCD
#define EC_REG_FAN1_DUTY    0xCE
#define EC_REG_FAN2_DUTY    0xCF
#define EC_REG_FAN1_RPM_HI  0xD0
#define EC_REG_FAN2_RPM_HI  0xD2
#define RPM_CONST           2156220   /* RPM = RPM_CONST / raw (fan-control) */

#ifndef DCC_FAN_VERSION
#define DCC_FAN_VERSION "dev"
#endif

static struct platform_device *dcc_pdev;
static bool have_wmi;

static int ec_byte(u8 reg, u8 *val)
{
	return ec_read(reg, val);
}

static int ec_rpm(u8 hi_reg, long *rpm)
{
	u8 hi, lo;
	int err = ec_byte(hi_reg, &hi);

	if (!err)
		err = ec_byte(hi_reg + 1, &lo);
	if (err)
		return err;
	*rpm = ((hi << 8) | lo) ? RPM_CONST / ((hi << 8) | lo) : 0;
	return 0;
}

/* --- hwmon --- */

static int dcc_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	u8 b;
	int err;

	switch (type) {
	case hwmon_temp:
		err = ec_byte(channel == 0 ? EC_REG_CPU_TEMP : EC_REG_GPU_TEMP, &b);
		if (err)
			return err;
		*val = b * 1000L;   /* millidegrees */
		return 0;
	case hwmon_fan:
		return ec_rpm(channel == 0 ? EC_REG_FAN1_RPM_HI : EC_REG_FAN2_RPM_HI, val);
	case hwmon_pwm:
		err = ec_byte(channel == 0 ? EC_REG_FAN1_DUTY : EC_REG_FAN2_DUTY, &b);
		if (err)
			return err;
		*val = b;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int dcc_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				 u32 attr, int channel, const char **str)
{
	*str = channel == 0 ? "CPU" : "GPU";
	return 0;
}

static umode_t dcc_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	return 0444;   /* read-only prototype */
}

static const struct hwmon_channel_info *const dcc_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_ops dcc_hwmon_ops = {
	.is_visible = dcc_hwmon_is_visible,
	.read = dcc_hwmon_read,
	.read_string = dcc_hwmon_read_string,
};

static const struct hwmon_chip_info dcc_hwmon_chip = {
	.ops = &dcc_hwmon_ops,
	.info = dcc_hwmon_info,
};

/* --- Clevo WMI "get" commands, raw --- */

static int clevo_wmi_get(u8 cmd, u32 *result)
{
	u32 arg = 0;
	struct acpi_buffer in = { sizeof(arg), &arg };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	status = wmi_evaluate_method(CLEVO_WMI_METHOD_GUID, 0, cmd, &in, &out);
	if (ACPI_FAILURE(status))
		return -EIO;
	obj = out.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		*result = (u32)obj->integer.value;
	else
		ret = -ENODATA;
	kfree(obj);
	return ret;
}

static ssize_t wmi_faninfo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const u8 cmds[] = { CLEVO_CMD_GET_FANINFO1, CLEVO_CMD_GET_FANINFO2, CLEVO_CMD_GET_FANINFO3 };
	ssize_t len = 0;
	int i;

	if (!have_wmi)
		return sysfs_emit(buf, "no Clevo WMI interface\n");
	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		u32 v = 0;
		int err = clevo_wmi_get(cmds[i], &v);

		if (err)
			len += sysfs_emit_at(buf, len, "faninfo%d (0x%02x): error %d\n", i + 1, cmds[i], err);
		else
			len += sysfs_emit_at(buf, len, "faninfo%d (0x%02x): 0x%08x\n", i + 1, cmds[i], v);
	}
	return len;
}
static DEVICE_ATTR_RO(wmi_faninfo);

/* EC registers this module reads, for comparison with the WMI values */
static ssize_t ec_values_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 t1 = 0, t2 = 0, d1 = 0, d2 = 0;
	long r1 = 0, r2 = 0;
	int err = ec_byte(EC_REG_CPU_TEMP, &t1) ?: ec_byte(EC_REG_GPU_TEMP, &t2) ?:
		  ec_byte(EC_REG_FAN1_DUTY, &d1) ?: ec_byte(EC_REG_FAN2_DUTY, &d2) ?:
		  ec_rpm(EC_REG_FAN1_RPM_HI, &r1) ?: ec_rpm(EC_REG_FAN2_RPM_HI, &r2);

	if (err)
		return sysfs_emit(buf, "EC read error %d\n", err);
	return sysfs_emit(buf, "cpu_temp=%u gpu_temp=%u fan1_duty=%u fan2_duty=%u fan1_rpm=%ld fan2_rpm=%ld\n",
			  t1, t2, d1, d2, r1, r2);
}
static DEVICE_ATTR_RO(ec_values);

static struct attribute *dcc_attrs[] = {
	&dev_attr_wmi_faninfo.attr,
	&dev_attr_ec_values.attr,
	NULL
};
ATTRIBUTE_GROUPS(dcc);

/* --- module --- */

static int dcc_probe(struct platform_device *pdev)
{
	struct device *hw;
	u8 probe;

	if (ec_read(EC_REG_CPU_TEMP, &probe)) {
		dev_err(&pdev->dev, "no ACPI EC access\n");
		return -ENODEV;
	}
	hw = devm_hwmon_device_register_with_info(&pdev->dev, DCC_FAN_NAME, NULL, &dcc_hwmon_chip, NULL);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	dev_info(&pdev->dev, "read-only, EC via ACPI EC driver, Clevo WMI %s\n",
		 have_wmi ? "present" : "not found");
	return 0;
}

static struct platform_driver dcc_driver = {
	.driver = {
		.name = DCC_FAN_NAME,
		.dev_groups = dcc_groups,
	},
	.probe = dcc_probe,
};

static int __init dcc_fan_init(void)
{
	int err;

	have_wmi = wmi_has_guid(CLEVO_WMI_METHOD_GUID);
	err = platform_driver_register(&dcc_driver);
	if (err)
		return err;
	dcc_pdev = platform_device_register_simple(DCC_FAN_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(dcc_pdev)) {
		platform_driver_unregister(&dcc_driver);
		return PTR_ERR(dcc_pdev);
	}
	return 0;
}

static void __exit dcc_fan_exit(void)
{
	platform_device_unregister(dcc_pdev);
	platform_driver_unregister(&dcc_driver);
}

module_init(dcc_fan_init);
module_exit(dcc_fan_exit);

MODULE_DESCRIPTION("Fan and temperature access for Clevo notebooks (Dudes Control Center)");
MODULE_AUTHOR("Dudes Control Center");
MODULE_LICENSE("GPL");
MODULE_VERSION(DCC_FAN_VERSION);
