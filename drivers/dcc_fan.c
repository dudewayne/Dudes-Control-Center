// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc_fan - fan and temperature access for Clevo notebooks (Dudes Control Center)
 *
 * Used by dcc-fan-daemon and dcc-fan-cli when loaded (1.36.0); they fall
 * back to direct port I/O without it. Loads only where the Clevo WMI
 * interface exists, unless force=1.
 *
 *  - Reads the EC through the kernel's ACPI EC driver (ec_read), which holds
 *    the EC lock: no race with firmware/ACPI accesses, unlike user-space
 *    port I/O on 0x62/0x66, and no ioperm (works under kernel lockdown).
 *  - Registers a hwmon device "dcc_fan": temp1 (CPU, EC 0x07), temp2 (GPU,
 *    EC 0xCD), fan1/fan2 (RPM from EC 0xD0/0xD1, 0xD2/0xD3), pwm1/pwm2
 *    (fan duty 0-255; writable by root).
 *  - Duty read: from Clevo WMI GET_FANINFO1/2 (0x63/0x64; byte 0 = duty of
 *    that fan, byte 1 = a second temperature, byte 2 = the temperature the
 *    fan follows: CPU for fan 1, GPU for fan 2; measured on an X58xWNx).
 *    The EC duty registers 0xCE/0xCF stay 0 on ECs driven by command 0x99.
 *  - Duty write ("method"): "wmi" = Clevo WMI SET_FANSPEED_VALUE (0x68,
 *    one byte per fan: bits 0-7 fan 1, 8-15 fan 2, 16-23 fan 3), "ec" = EC
 *    command 0x99 <fan> <duty> through ec_transaction (what the fan daemon
 *    does with port I/O), "auto" = WMI if present, else EC.
 *  - Watchdog: once a duty was written, the fans go to 100% when no write
 *    comes for watchdog_sec seconds (the daemon stopped or hangs), and when
 *    the module is unloaded.
 *  - wmi_faninfo / ec_values: raw values for comparison and debugging.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#define DCC_FAN_NAME "dcc_fan"
#define CLEVO_WMI_METHOD_GUID "ABBC0F6D-8EA1-11D1-00A0-C90629100000"
#define CLEVO_CMD_GET_FANINFO1 0x63
#define CLEVO_CMD_GET_FANINFO2 0x64
#define CLEVO_CMD_GET_FANINFO3 0x6e
#define CLEVO_CMD_SET_FANSPEED_VALUE 0x68
#define EC_CMD_SET_FAN 0x99
#define DCC_NUM_FANS 2

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

static char *method = "auto";
module_param(method, charp, 0444);
MODULE_PARM_DESC(method, "How the fan speed is set: auto (WMI if present, else EC), wmi, ec");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Load without the Clevo WMI interface (EC only)");

static int watchdog_sec = 30;
module_param(watchdog_sec, int, 0644);
MODULE_PARM_DESC(watchdog_sec, "Fans to 100% when no duty was written for this many seconds (0 = off)");

static DEFINE_MUTEX(dcc_lock);          /* WMI calls, EC writes, duty state */
static u8 duty_set[DCC_NUM_FANS];       /* last duty written per fan */
static bool controlled;                 /* a duty was written since load */
static bool failsafe;                   /* the watchdog set 100% */
static bool use_wmi;                    /* writes go through WMI (else EC) */
static void dcc_watchdog_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(dcc_watchdog, dcc_watchdog_fn);

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

static int clevo_wmi_get(u8 cmd, u32 *result);
static int clevo_wmi_set(u8 cmd, u32 arg);

/* Real duty of fan ch (0/1): WMI byte 0, else the EC duty register */
static int dcc_duty_get(int ch, long *val)
{
	u8 b;
	int err;

	if (have_wmi) {
		u32 v;

		mutex_lock(&dcc_lock);
		err = clevo_wmi_get(ch == 0 ? CLEVO_CMD_GET_FANINFO1 : CLEVO_CMD_GET_FANINFO2, &v);
		mutex_unlock(&dcc_lock);
		if (!err) {
			*val = v & 0xff;
			return 0;
		}
	}
	err = ec_byte(ch == 0 ? EC_REG_FAN1_DUTY : EC_REG_FAN2_DUTY, &b);
	if (err)
		return err;
	*val = b;
	return 0;
}

/* Send duty_set[] to the hardware; called with dcc_lock held */
static int dcc_apply_locked(void)
{
	int err, i;

	if (use_wmi) {
		u32 arg = duty_set[0] | (duty_set[1] << 8);

		err = clevo_wmi_set(CLEVO_CMD_SET_FANSPEED_VALUE, arg);
	} else {
		err = 0;
		for (i = 0; i < DCC_NUM_FANS && !err; i++) {
			u8 data[2] = { i + 1, duty_set[i] };

			err = ec_transaction(EC_CMD_SET_FAN, data, 2, NULL, 0);
		}
	}
	return err;
}

static int dcc_duty_set(int ch, long val)
{
	int err;

	if (val < 0 || val > 255)
		return -EINVAL;
	mutex_lock(&dcc_lock);
	duty_set[ch] = (u8)val;   /* the other fan keeps duty_set (read at load) */
	controlled = true;
	if (failsafe) {
		dev_info(&dcc_pdev->dev, "duty written again, watchdog fail-safe ended\n");
		failsafe = false;
	}
	err = dcc_apply_locked();
	mutex_unlock(&dcc_lock);
	if (watchdog_sec > 0)
		mod_delayed_work(system_wq, &dcc_watchdog, (unsigned long)watchdog_sec * HZ);
	return err;
}

/* No duty written for watchdog_sec: the controlling program is gone */
static void dcc_watchdog_fn(struct work_struct *work)
{
	mutex_lock(&dcc_lock);
	if (controlled && watchdog_sec > 0) {
		duty_set[0] = duty_set[1] = 255;
		failsafe = true;
		if (dcc_apply_locked())
			dev_err(&dcc_pdev->dev, "watchdog: could not set the fans to 100%%\n");
		else
			dev_warn(&dcc_pdev->dev, "watchdog: no fan update for %d s, fans set to 100%%\n",
				 watchdog_sec);
	}
	mutex_unlock(&dcc_lock);
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
		return dcc_duty_get(channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int dcc_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return dcc_duty_set(channel, val);
	return -EOPNOTSUPP;
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
	return (type == hwmon_pwm) ? 0644 : 0444;   /* only root sets the fans */
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
	.write = dcc_hwmon_write,
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

static int clevo_wmi_set(u8 cmd, u32 arg)
{
	struct acpi_buffer in = { sizeof(arg), &arg };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status = wmi_evaluate_method(CLEVO_WMI_METHOD_GUID, 0, cmd, &in, &out);

	kfree(out.pointer);
	return ACPI_FAILURE(status) ? -EIO : 0;
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
		int err;

		mutex_lock(&dcc_lock);
		err = clevo_wmi_get(cmds[i], &v);
		mutex_unlock(&dcc_lock);

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

/* method in use, last written duties, watchdog state */
static ssize_t control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&dcc_lock);
	n = sysfs_emit(buf, "method=%s controlled=%d duty_set=%u,%u watchdog_sec=%d failsafe=%d\n",
		       use_wmi ? "wmi" : "ec", controlled, duty_set[0], duty_set[1], watchdog_sec, failsafe);
	mutex_unlock(&dcc_lock);
	return n;
}
static DEVICE_ATTR_RO(control);

static struct attribute *dcc_attrs[] = {
	&dev_attr_control.attr,
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
	/* start from the fans' current duty, so writing one fan leaves the other */
	for (int i = 0; i < DCC_NUM_FANS; i++) {
		long d;

		if (!dcc_duty_get(i, &d))
			duty_set[i] = (u8)d;
	}
	hw = devm_hwmon_device_register_with_info(&pdev->dev, DCC_FAN_NAME, NULL, &dcc_hwmon_chip, NULL);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	dev_info(&pdev->dev, "EC via ACPI EC driver, Clevo WMI %s, fan speed set through %s, watchdog %d s\n",
		 have_wmi ? "present" : "not found", use_wmi ? "WMI" : "EC command 0x99", watchdog_sec);
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
	if (!have_wmi && !force) {
		pr_info(DCC_FAN_NAME ": no Clevo WMI interface, not loading (force=1 to use the EC anyway)\n");
		return -ENODEV;
	}
	if (!strcmp(method, "wmi") && !have_wmi) {
		pr_err(DCC_FAN_NAME ": method=wmi, but there is no Clevo WMI interface\n");
		return -ENODEV;
	}
	if (strcmp(method, "auto") && strcmp(method, "wmi") && strcmp(method, "ec")) {
		pr_err(DCC_FAN_NAME ": unknown method '%s' (auto, wmi, ec)\n", method);
		return -EINVAL;
	}
	use_wmi = !strcmp(method, "wmi") || (!strcmp(method, "auto") && have_wmi);
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
	cancel_delayed_work_sync(&dcc_watchdog);
	mutex_lock(&dcc_lock);
	if (controlled) {   /* leave the fans safe, like the fan service's stop */
		duty_set[0] = duty_set[1] = 255;
		dcc_apply_locked();
		pr_info(DCC_FAN_NAME ": unloaded, fans set to 100%%\n");
	}
	mutex_unlock(&dcc_lock);
	platform_device_unregister(dcc_pdev);
	platform_driver_unregister(&dcc_driver);
}

module_init(dcc_fan_init);
module_exit(dcc_fan_exit);

MODULE_DESCRIPTION("Fan and temperature access for Clevo notebooks (Dudes Control Center)");
MODULE_AUTHOR("Dudes Control Center");
MODULE_LICENSE("GPL");
MODULE_VERSION(DCC_FAN_VERSION);
