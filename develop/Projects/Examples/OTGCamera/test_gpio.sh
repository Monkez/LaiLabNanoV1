echo 595 > /sys/class/gpio/export
echo in > /sys/class/gpio/gpio595/direction
cat /sys/class/gpio/gpio595/value
