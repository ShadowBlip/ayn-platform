# Platform driver for AYN x86 handheldsds

This driver provides a hwmon interface for PWM control, as well as RGB
control and access to temperature sensotrs provided by the system EC


Supported devices include:

 - AYN Loki Max

## Build
If you only want to build and test the module (you need headers for your
kernel):

```shell
$ git clone https://github.com/ShadowBlip/ayn-platform.git
$ cd ayn-platform
$ make
```

Then insert the module and check `sensors` and `dmesg` if appropriate:
```shell
# insmod ayn-sensors.ko
$ sensors
```

## Install

You'll need appropriate headers for your kernel and `dkms` package from your
distribution.

```shell
$ git clone https://github.com/ShadowBlip/ayn-platform.git
$ cd ayn-platform
$ make
# make dkms
```

## Usage

Insert the module with `insmod`. Then look for a `hwmon` device with name
`aynec`, i.e.:

`$ cat /sys/class/hwmon/hwmon?/name`

### Reading fan RPM

`sensors` will show the fan RPM as read from the EC. You can also read the
file `fan1_input` to get the fan RPM.

### Controlling the fan

***Warning: controlling the fan without an accurate reading of the CPU, GPU,
and Battery temperature can cause irreversible damage to the device. Use at
your own risk!***

To enable manual control of the fan (assuming `hwmon5` is our driver, look for
`aynec` in the `name` file):

`# echo 1 > /sys/class/hwmon/hwmon5/pwm1_enable`

Then input values in the range `[0-255]` to the pwm:

`# echo 100 > /sys/class/hwmon/hwmon5/pwm1`

