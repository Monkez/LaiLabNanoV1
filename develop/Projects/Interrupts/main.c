/*
 * GPIO Interrupt (Polling) Example for LicheeRV Nano
 * Uses poll() on /sys/class/gpio/gpioX/value to detect edge events
 *
 * Reference: https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include "gpio.h"

#define BUTTON_PIN  505   /* Adjust to your actual button GPIO number */

int main() {
    char path[64];
    char val;

    printf("GPIO Interrupt — waiting for edges on pin %d\n", BUTTON_PIN);

    exportPin(BUTTON_PIN);
    setDirPin(BUTTON_PIN, "in");
    setEdge(BUTTON_PIN, "both");

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", BUTTON_PIN);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open gpio value");
        return 1;
    }

    /* Initial dummy read to clear any pending event */
    read(fd, &val, 1);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI | POLLERR;

    printf("Waiting for events (Ctrl+C to stop)...\n");
    int count = 0;

    while (1) {
        int ret = poll(&pfd, 1, 5000);  /* 5s timeout */
        if (ret < 0) {
            perror("poll");
            break;
        }
        if (ret == 0) {
            printf("... no event (timeout)\n");
            continue;
        }

        /* Seek back to beginning and read the new value */
        lseek(fd, 0, SEEK_SET);
        read(fd, &val, 1);
        count++;
        printf("[%d] Edge detected! Value: %c\n", count, val);
    }

    close(fd);
    unexportPin(BUTTON_PIN);
    return 0;
}
