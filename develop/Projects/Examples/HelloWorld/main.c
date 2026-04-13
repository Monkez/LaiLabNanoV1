#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define LED_PATH "/sys/class/leds/led-user/brightness"

int write_led(int value)
{
    int fd = open(LED_PATH, O_WRONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (value)
        write(fd, "1", 1);
    else
        write(fd, "0", 1);

    close(fd);
    return 0;
}

int main()
{
    time_t start = time(NULL);

    while (time(NULL) - start < 5) {
        write_led(1);      // LED ON
        usleep(500000);    // 0.5s

        write_led(0);      // LED OFF
        usleep(500000);    // 0.5s
    }

    // đảm bảo LED tắt khi kết thúc
    write_led(0);

    return 0;
}

