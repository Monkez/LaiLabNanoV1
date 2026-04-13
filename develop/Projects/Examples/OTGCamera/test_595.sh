#!/bin/sh
echo 595 > /sys/class/gpio/export 2>/tmp/gpio_err
echo in > /sys/class/gpio/gpio595/direction 2>>/tmp/gpio_err
cat /sys/class/gpio/gpio595/value 2>>/tmp/gpio_err
cat /tmp/gpio_err
