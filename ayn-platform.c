// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Ayn x86 Handhelds that expose fan reading and
 * control, as well as temperature sensor readings exposed by the EC
 * via hwmon sysfs.
 *
 * Fan control is provided via pwm interface in the range [0-255].
 * Ayn use [0-128] as the range in the EC, the written value is
 * scaled to accommodate. The EC also provides a configurable fan
 * curve with five set points that associate a temperature [0-100]
 * in Celcius with a fan speed [0-128]. The auto_point fan speeds
 * are scaled from the range [0-255]. Temperature readings are
 * scaled from the hwmon expected millidegrees to degrees when read.
 *
 * Copyright (C) 2023-2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/processor.h>

/* Handle ACPI lock mechanism */
static u32 ayn_mutex;

#define ACPI_LOCK_DELAY_MS 500

static bool lock_global_acpi_lock(void) {
  return ACPI_SUCCESS(acpi_acquire_global_lock(ACPI_LOCK_DELAY_MS, &ayn_mutex));
}

static bool unlock_global_acpi_lock(void) {
  return ACPI_SUCCESS(acpi_release_global_lock(ayn_mutex));
}

enum ayn_model {
  ayn_loki_max = 1,
  ayn_loki_minipro,
  ayn_loki_zero,
};

static enum ayn_model model;

/* EC Teperature Sensors */
#define AYN_SENSOR_BAT_TEMP_REG 0x04    /* Battery */
#define AYN_SENSOR_CHARGE_TEMP_REG 0x07 /* Charger IC */
#define AYN_SENSOR_MB_TEMP_REG 0x05     /* Motherboard */
#define AYN_SENSOR_PROC_TEMP_REG 0x09   /* CPU Core */
#define AYN_SENSOR_VCORE_TEMP_REG 0x08  /* vCore */

/* Fan reading and PWM */
#define AYN_SENSOR_PWM_FAN_MODE_REG 0x10  /* PWM operating mode */
#define AYN_SENSOR_PWM_FAN_SET_REG 0x11   /* PWM duty cycle */
#define AYN_SENSOR_PWM_FAN_SPEED_REG 0x20 /* Fan speed */

/* EC controlled fan curve registers */
#define AYN_SENSOR_PWM_FAN_SPEED_1_REG 0x12
#define AYN_SENSOR_PWM_FAN_SPEED_2_REG 0x14
#define AYN_SENSOR_PWM_FAN_SPEED_3_REG 0x16
#define AYN_SENSOR_PWM_FAN_SPEED_4_REG 0x18
#define AYN_SENSOR_PWM_FAN_SPEED_5_REG 0x1A
#define AYN_SENSOR_PWM_FAN_TEMP_1_REG 0x13
#define AYN_SENSOR_PWM_FAN_TEMP_2_REG 0x15
#define AYN_SENSOR_PWM_FAN_TEMP_3_REG 0x17
#define AYN_SENSOR_PWM_FAN_TEMP_4_REG 0x19
#define AYN_SENSOR_PWM_FAN_TEMP_5_REG 0x1B

/* EC Controlled RGB registers */
#define AYN_LED_MC_B_REG 0xB2 /* Blue, range 0x00-0xFF */
#define AYN_LED_MC_G_REG 0xB1 /* Green, range 0x00-0xFF */
#define AYN_LED_MC_R_REG 0xB0 /* Red, range 0x00-0xFF */
#define AYN_LED_MODE_REG 0xB3 /* RGB Mode */

/* RGB Mode values */
#define AYN_LED_MODE_BREATH 0x00        /* Default breathing mode */
#define AYN_LED_MODE_WRITE 0xAA         /* User defined mode */
#define AYN_LED_MODE_WRITE_ENABLED 0x55 /* Return value when probed */

enum led_mode {
  breath = 0,
  write,
};

static const struct dmi_system_id dmi_table[] = {
    {
        .matches =
            {
                DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "ayn"),
                DMI_EXACT_MATCH(DMI_BOARD_NAME, "Loki Max"),
            },
        .driver_data = (void *)ayn_loki_max,
    },
    {
        .matches =
            {
                DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "ayn"),
                DMI_EXACT_MATCH(DMI_BOARD_NAME, "Loki MiniPro"),
            },
        .driver_data = (void *)ayn_loki_minipro,
    },
    {
        .matches =
            {
                DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "ayn"),
                DMI_EXACT_MATCH(DMI_BOARD_NAME, "Loki Zero"),
            },
        .driver_data = (void *)ayn_loki_zero,
    },
    {},
};

/* Helper functions to handle EC read/write */
static int read_from_ec(u8 reg, int size, long *val) {
  int i;
  int ret;
  u8 buffer;

  if (!lock_global_acpi_lock())
    return -EBUSY;

  *val = 0;
  for (i = 0; i < size; i++) {
    ret = ec_read(reg + i, &buffer);
    if (ret)
      return ret;
    *val <<= i * 8;
    *val += buffer;
  }

  if (!unlock_global_acpi_lock())
    return -EBUSY;

  return 0;
}

static int write_to_ec(u8 reg, u8 val) {
  int ret;

  if (!lock_global_acpi_lock())
    return -EBUSY;

  ret = ec_write(reg, val);

  if (!unlock_global_acpi_lock())
    return -EBUSY;

  return ret;
}

/* Thermal Sensor Functions*/
struct thermal_sensor {
  char *name;
  int reg;
};

static struct thermal_sensor thermal_sensors[] = {
    {"Battery", AYN_SENSOR_BAT_TEMP_REG},
    {"Motherboard", AYN_SENSOR_MB_TEMP_REG},
    {"Charger IC", AYN_SENSOR_CHARGE_TEMP_REG},
    {"vCore", AYN_SENSOR_VCORE_TEMP_REG},
    {"CPU Core", AYN_SENSOR_PROC_TEMP_REG},
    {
        0,
    }};

static long thermal_sensor_temp(u8 reg, long *val) {
  long retval;
  retval = read_from_ec(reg, 1, val);
  if (retval)
    return retval;
  *val = *val * (long)1000; // convert from hwmon expected millidegree to degree
  return retval;
};

static ssize_t thermal_sensor_show(struct device *dev,
                                   struct device_attribute *attr, char *buf) {
  int index;
  long retval;
  long val;
  index = to_sensor_dev_attr(attr)->index;
  retval = thermal_sensor_temp(thermal_sensors[index].reg, &val);
  if (retval)
    return retval;
  return sprintf(buf, "%ld\n", val);
}

static ssize_t thermal_sensor_label(struct device *dev,
                                    struct device_attribute *attr, char *buf) {
  int index;
  index = to_sensor_dev_attr(attr)->index;
  return sprintf(buf, "%s\n", thermal_sensors[index].name);
}

/* PWM mode functions */
/* Callbacks for pwm_auto_point attributes */
static ssize_t pwm_curve_store(struct device *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count) {
  int index;
  int retval;
  int val;
  u8 reg;

  retval = kstrtoint(buf, 0, &val);
  if (retval)
    return retval;

  index = to_sensor_dev_attr(attr)->index;
  switch (index) {
  case 0:
    reg = AYN_SENSOR_PWM_FAN_SPEED_1_REG;
    break;
  case 1:
    reg = AYN_SENSOR_PWM_FAN_SPEED_2_REG;
    break;
  case 2:
    reg = AYN_SENSOR_PWM_FAN_SPEED_3_REG;
    break;
  case 3:
    reg = AYN_SENSOR_PWM_FAN_SPEED_4_REG;
    break;
  case 4:
    reg = AYN_SENSOR_PWM_FAN_SPEED_5_REG;
    break;
  case 5:
    reg = AYN_SENSOR_PWM_FAN_TEMP_1_REG;
    break;
  case 6:
    reg = AYN_SENSOR_PWM_FAN_TEMP_2_REG;
    break;
  case 7:
    reg = AYN_SENSOR_PWM_FAN_TEMP_3_REG;
    break;
  case 8:
    reg = AYN_SENSOR_PWM_FAN_TEMP_4_REG;
    break;
  case 9:
    reg = AYN_SENSOR_PWM_FAN_TEMP_5_REG;
    break;
  default:
    return -EINVAL;
  }

  switch (index) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
    if (val < 0 || val > 255)
      return -EINVAL;
    val = val >> 1;
    break;
  case 5:
  case 6:
  case 7:
  case 8:
  case 9:
    if (val < 0 || val > 100)
      return -EINVAL;
    break;
  default:
    return -EINVAL;
  }

  retval = write_to_ec(reg, val);
  if (retval)
    return retval;
  return count;
}

static ssize_t pwm_curve_show(struct device *dev, struct device_attribute *attr,
                              char *buf) {
  int index;
  int retval;
  long val;
  u8 reg;

  index = to_sensor_dev_attr(attr)->index;
  switch (index) {
  case 0:
    reg = AYN_SENSOR_PWM_FAN_SPEED_1_REG;
    break;
  case 1:
    reg = AYN_SENSOR_PWM_FAN_SPEED_2_REG;
    break;
  case 2:
    reg = AYN_SENSOR_PWM_FAN_SPEED_3_REG;
    break;
  case 3:
    reg = AYN_SENSOR_PWM_FAN_SPEED_4_REG;
    break;
  case 4:
    reg = AYN_SENSOR_PWM_FAN_SPEED_5_REG;
    break;
  case 5:

    reg = AYN_SENSOR_PWM_FAN_TEMP_1_REG;
    break;
  case 6:
    reg = AYN_SENSOR_PWM_FAN_TEMP_2_REG;
    break;
  case 7:
    reg = AYN_SENSOR_PWM_FAN_TEMP_3_REG;
    break;
  case 8:
    reg = AYN_SENSOR_PWM_FAN_TEMP_4_REG;
    break;
  case 9:
    reg = AYN_SENSOR_PWM_FAN_TEMP_5_REG;
    break;
  default:
    return -EINVAL;
  }

  retval = read_from_ec(reg, 1, &val);
  if (retval)
    return retval;

  switch (index) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
    val = val << 1;
    break;
  default:
    break;
  }

  return sysfs_emit(buf, "%ld\n", val);
}

/* Manual provides direct control of the PWM */
static int ayn_pwm_manual(void) {
  return write_to_ec(AYN_SENSOR_PWM_FAN_MODE_REG, 0x00);
}

/* Auto provides EC full control of the PWM */
static int ayn_pwm_auto(void) {
  return write_to_ec(AYN_SENSOR_PWM_FAN_MODE_REG, 0x01);
}

/* User defined mode allows users to set a custom 5 point
 * fan curve in the EC which uses the CPU temperature. */
static int ayn_pwm_user(void) {
  return write_to_ec(AYN_SENSOR_PWM_FAN_MODE_REG, 0x02);
}

/* Temperature sensor and fan curve attributes */
static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, thermal_sensor_show, NULL, 0);
static SENSOR_DEVICE_ATTR(temp1_label, S_IRUGO, thermal_sensor_label, NULL, 0);
static SENSOR_DEVICE_ATTR(temp2_input, S_IRUGO, thermal_sensor_show, NULL, 1);
static SENSOR_DEVICE_ATTR(temp2_label, S_IRUGO, thermal_sensor_label, NULL, 1);
static SENSOR_DEVICE_ATTR(temp3_input, S_IRUGO, thermal_sensor_show, NULL, 2);
static SENSOR_DEVICE_ATTR(temp3_label, S_IRUGO, thermal_sensor_label, NULL, 2);
static SENSOR_DEVICE_ATTR(temp4_input, S_IRUGO, thermal_sensor_show, NULL, 3);
static SENSOR_DEVICE_ATTR(temp4_label, S_IRUGO, thermal_sensor_label, NULL, 3);
static SENSOR_DEVICE_ATTR(temp5_input, S_IRUGO, thermal_sensor_show, NULL, 4);
static SENSOR_DEVICE_ATTR(temp5_label, S_IRUGO, thermal_sensor_label, NULL, 4);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_pwm, pwm_curve, 0);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_pwm, pwm_curve, 1);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_pwm, pwm_curve, 2);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_pwm, pwm_curve, 3);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point5_pwm, pwm_curve, 4);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_temp, pwm_curve, 5);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_temp, pwm_curve, 6);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_temp, pwm_curve, 7);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_temp, pwm_curve, 8);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point5_temp, pwm_curve, 9);

static struct attribute *ayn_sensors_attrs[] = {
    &sensor_dev_attr_temp1_input.dev_attr.attr,
    &sensor_dev_attr_temp1_label.dev_attr.attr,
    &sensor_dev_attr_temp2_input.dev_attr.attr,
    &sensor_dev_attr_temp2_label.dev_attr.attr,
    &sensor_dev_attr_temp3_input.dev_attr.attr,
    &sensor_dev_attr_temp3_label.dev_attr.attr,
    &sensor_dev_attr_temp4_input.dev_attr.attr,
    &sensor_dev_attr_temp4_label.dev_attr.attr,
    &sensor_dev_attr_temp5_input.dev_attr.attr,
    &sensor_dev_attr_temp5_label.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point5_pwm.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
    &sensor_dev_attr_pwm1_auto_point5_temp.dev_attr.attr,
    NULL,
};

ATTRIBUTE_GROUPS(ayn_sensors);

/* Callbacks for fan1/pwm attributes */
static umode_t ayn_ec_hwmon_is_visible(const void *drvdata,
                                       enum hwmon_sensor_types type, u32 attr,
                                       int channel) {
  switch (type) {
  case hwmon_fan:
    return 0444;
  case hwmon_pwm:
    return 0644;
  default:
    return 0;
  }
}

static int ayn_platform_read(struct device *dev, enum hwmon_sensor_types type,
                             u32 attr, int channel, long *val) {
  int ret;

  switch (type) {
  case hwmon_fan:
    switch (attr) {
    case hwmon_fan_input:
      return read_from_ec(AYN_SENSOR_PWM_FAN_SPEED_REG, 2, val);
    default:
      break;
    }
    break;
  case hwmon_pwm:
    switch (attr) {
    case hwmon_pwm_input:
      ret = read_from_ec(AYN_SENSOR_PWM_FAN_SET_REG, 1, val);
      if (ret)
        return ret;
      switch (model) {
      case ayn_loki_max:
      case ayn_loki_minipro:
      case ayn_loki_zero:
        *val = *val << 1; /* EC max value is 128 */
        break;
      default:
        break;
      }
      return 0;
    case hwmon_pwm_mode:
      ret = read_from_ec(AYN_SENSOR_PWM_FAN_MODE_REG, 1, val);
      switch (*val) {
      /* EC uses 0 for manual and 1 for automatic,
       * reflect hwmon usage instead */
      case 0:
        *val = 1;
        break;
      case 1:
        *val = 0;
        break;
      default:
        break;
      }
      return ret;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return -EOPNOTSUPP;
}

static int ayn_platform_write(struct device *dev, enum hwmon_sensor_types type,
                              u32 attr, int channel, long val) {
  switch (type) {
  case hwmon_pwm:
    switch (attr) {
    case hwmon_pwm_mode:
      if (val == 1)
        return ayn_pwm_manual();
      else if (val == 2)
        return ayn_pwm_user();
      else if (val == 0)
        return ayn_pwm_auto();
      return -EINVAL;
    case hwmon_pwm_input:
      if (val < 0 || val > 255)
        return -EINVAL;
      switch (model) {
      case ayn_loki_max:
      case ayn_loki_minipro:
      case ayn_loki_zero:
        val = val >> 1; /* EC max value is 128 */
        break;
      default:
        break;
      }
      return write_to_ec(AYN_SENSOR_PWM_FAN_SET_REG, val);
    default:
      break;
    }
    break;
  default:
    break;
  }
  return -EOPNOTSUPP;
}

/* RGB LED Logic */
static ssize_t led_mode_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count) {
  int val;
  int retval;
  int mode;

  retval = kstrtoint(buf, 0, &val);
  if (retval)
    return retval;

  if (val) {
    mode = AYN_LED_MODE_WRITE;
  } else {
    mode = AYN_LED_MODE_BREATH;
  }

  retval = write_to_ec(AYN_LED_MODE_REG, mode);
  if (retval)
    return retval;

  return count;
};

static ssize_t led_mode_show(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  long mode;
  int val;
  int retval;

  retval = read_from_ec(AYN_LED_MODE_REG, 1, &mode);
  if (retval)
    return retval;
  switch (mode) {
  case AYN_LED_MODE_BREATH:
    val = breath;
    break;
  case AYN_LED_MODE_WRITE:
  case AYN_LED_MODE_WRITE_ENABLED:
    val = write;
    break;
  default:
    break;
  }
  return sysfs_emit(buf, "%d\n", val);
};

static DEVICE_ATTR_RW(led_mode);

static void ayn_led_mc_brightness_set(struct led_classdev *led_cdev,
                                      enum led_brightness brightness) {
  struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
  long mode;
  int retval;
  int val;
  int i;
  struct mc_subled s_led;

  led_cdev->brightness = brightness;
  retval = read_from_ec(AYN_LED_MODE_REG, 1, &mode);

  if (retval)
    return;

  switch (mode) {
  case AYN_LED_MODE_WRITE:
  case AYN_LED_MODE_WRITE_ENABLED:
    break;
  default:
    return;
  }

  for (i = 0; i < mc_cdev->num_colors; i++) {
    s_led = mc_cdev->subled_info[i];
    val = brightness * s_led.intensity / led_cdev->max_brightness;
    write_to_ec(s_led.channel, val);
  }

  retval = write_to_ec(AYN_LED_MODE_REG, AYN_LED_MODE_WRITE);
};

static enum led_brightness
ayn_led_mc_brightness_get(struct led_classdev *led_cdev) {
  return led_cdev->brightness;
};

static struct attribute *ayn_led_mc_attrs[] = {
    &dev_attr_led_mode.attr,
    NULL,
};

ATTRIBUTE_GROUPS(ayn_led_mc);

/* Initialization logic */
static const struct hwmon_channel_info *ayn_platform_sensors[] = {
    HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
    HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_MODE),
    NULL,
};

static const struct hwmon_ops ayn_ec_hwmon_ops = {
    .is_visible = ayn_ec_hwmon_is_visible,
    .read = ayn_platform_read,
    .write = ayn_platform_write,
};

static const struct hwmon_chip_info ayn_ec_chip_info = {
    .ops = &ayn_ec_hwmon_ops,
    .info = ayn_platform_sensors,
};

struct mc_subled ayn_led_mc_subled_info[] = {
    {
        .color_index = LED_COLOR_ID_RED,
        .brightness = 0,
        .intensity = 0,
        .channel = AYN_LED_MC_R_REG,
    },
    {
        .color_index = LED_COLOR_ID_GREEN,
        .brightness = 0,
        .intensity = 0,
        .channel = AYN_LED_MC_G_REG,
    },
    {
        .color_index = LED_COLOR_ID_BLUE,
        .brightness = 0,
        .intensity = 0,
        .channel = AYN_LED_MC_B_REG,
    },
};

struct led_classdev_mc ayn_led_mc = {
    .led_cdev =
        {
            .name = "multicolor:chassis",
            .brightness = 0,
            .max_brightness = 255,
            .brightness_set = ayn_led_mc_brightness_set,
            .brightness_get = ayn_led_mc_brightness_get,
        },
    .num_colors = ARRAY_SIZE(ayn_led_mc_subled_info),
    .subled_info = ayn_led_mc_subled_info,
};

static int ayn_platform_probe(struct platform_device *pdev) {
  struct device *dev = &pdev->dev;
  struct device *hwdev;
  int ret;

  ret = devm_led_classdev_multicolor_register(dev, &ayn_led_mc);
  if (ret)
    return ret;

  struct device *led_dev = ayn_led_mc.led_cdev.dev;

  ret = devm_device_add_groups(led_dev, ayn_led_mc_groups);
  if (ret)
    return ret;

  hwdev = devm_hwmon_device_register_with_info(
      dev, "aynec", NULL, &ayn_ec_chip_info, ayn_sensors_groups);
  return PTR_ERR_OR_ZERO(hwdev);
}

static struct platform_driver ayn_platform_driver = {
    .driver =
        {
            .name = "ayn-platform",
        },
    .probe = ayn_platform_probe,
};

static struct platform_device *ayn_platform_device;

static int __init ayn_platform_init(void) {
  ayn_platform_device = platform_create_bundle(
      &ayn_platform_driver, ayn_platform_probe, NULL, 0, NULL, 0);

  return PTR_ERR_OR_ZERO(ayn_platform_device);
}

static void __exit ayn_platform_exit(void) {
  platform_device_unregister(ayn_platform_device);
  platform_driver_unregister(&ayn_platform_driver);
}

MODULE_DEVICE_TABLE(dmi, dmi_table);

module_init(ayn_platform_init);
module_exit(ayn_platform_exit);

MODULE_AUTHOR("Derek John Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION(
    "Platform driver that handles EC sensors of Ayn x86 devices");
MODULE_LICENSE("GPL");
