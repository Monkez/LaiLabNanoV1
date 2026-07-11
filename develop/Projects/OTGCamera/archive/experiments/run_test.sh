#!/usr/bin/expect
spawn ssh root@192.168.100.2
expect "password:"
send "milkv\r"
expect "#"
send "echo 595 > /sys/class/gpio/export 2>&1\r"
expect "#"
send "echo in > /sys/class/gpio/gpio595/direction 2>&1\r"
expect "#"
send "cat /sys/class/gpio/gpio595/value 2>&1\r"
expect "#"
send "exit\r"
expect eof
