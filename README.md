# leds-pwmn Linux Kernel Driver

Current kernel lacks a good way to control color leds. The only possibility is to
express each color as individual led. Triggers can get easilly out of sync.

This driver allows to create single led with multiple pwm channels and control each
channel separately in sysfs.
