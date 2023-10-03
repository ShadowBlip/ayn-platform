# Platform driver for AYN x86 handhelds

This driver provides a hwmon interface for PWM control, as well as access 
to temperature sensors provided by the system EC

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
# insmod ayn-platform.ko
$ sensors
aynec-isa-0000
Adapter: ISA adapter
fan1:        3007 RPM
Battery:      +29.0°C
Motherboard:  +41.0°C
Charger IC:   +38.0°C
vCore:        +48.0°C
CPU Core:     +48.0°C
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

### Fan Control

***Warning: controlling the fan without an accurate reading of the CPU, GPU,
and Battery temperature can cause irreversible damage to the device. Use at
your own risk!***

#### Automatic Control
This will use the BIOS default fan curve and is the default setting of the EC.

To enable automatic control of the fan (assuming `hwmon5` is our driver, look for
`aynec` in the `name` file):

`# echo 0 > /sys/class/hwmon/hwmon5/pwm1_mode`

#### Manual Control
This mode is useful to explicitly set a fan speed, or with the use of userspace
tools that adjust fan speed using a custom fan curve defined in software.

To enable manual control of the fan (assuming `hwmon5` is our driver, look for
`aynec` in the `name` file):

`# echo 1 > /sys/class/hwmon/hwmon5/pwm1_mode`

Then input values in the range `[0-255]` to the pwm:

`# echo 100 > /sys/class/hwmon/hwmon5/pwm1`

#### User Defined Control
This mode allows the user to override the default BIOS fan curve with a user
defined fan curve. There are 5 set point pairs for temperature and fan speed.
The temperature value is a cutoff for that set point, any CPU temperature
below that point and above the lower set point will run at that set points
fan speed. Temperature is in degrees Celsius.

To enable user defined control of the fan (assuming `hwmon5` is our driver,
look for `aynec` in the `name` file):

`# echo 2 > /sys/class/hwmon/hwmon5/pwm1_mode`

Set an input value in the range `[0-255]` to the pwm:

`# echo 100 > /sys/class/hwmon/hwmon5/pwm1_auto_point1_pwm`

Set an input value in the range `[0-100]` to the temp:

`# echo 50 > /sys/class/hwmon/hwmon5/pwm1_auto_point1_temp`

### RGB Control
RGB control is available using the character files found in the following location:
`/sys/class/leds/multicolor:chassis/` . Writing to the files within this directory
can be done using the pattern `echo <value> | sudo tee <path>`

#### Mode Setting
There are two modes available, `Rainbow Breathing` is represented by the value `0`
and `Manual` is represented by the value `1`. When in Rainbow Breathing mode,
brightness and color intensity changes are ignored. You must be in Manual mode to
adjust the color or brightness of the LED's.

To change modes:
`echo 1 | sudo tee /sys/class/leds/multicolor:chassis/device/led_mode`

#### Identifying and Setting Color Values
You can read the `brightness`, `multi_index`, and `multi_intensity` from their
respective files in `/sys/class/leds/multicolor:chassis/` . Each do the following:
brightness: The current average luminosity of all three colors in the range `[0-255]`.
Setting this to `0` will turn off the LED's while setting it to `255` represents the
maximum luminosity of the selected multi_intensity range.
multi_index: Displays the order of the colors when setting multi_intensity. Set to
`red green blue`.
multi_intensity: The current intensity of each individual color [0-255] where 0 is off
and 255 is most luminous. The value is dispalyed and set with three numbers separated
by a single space. Each of these values is multiplied by the current brightness and
divided by the maximum brightness when setting the true value. A setting of `0 0 0` is
off and `255 255 255` represents all colors at maximum intensity.
