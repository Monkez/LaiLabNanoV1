/*
 * GPIO Control Library for LicheeRV Nano
 * Uses Linux sysfs interface (/sys/class/gpio/)
 *
 * Reference: https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110
 */
#ifndef GPIO_H
#define GPIO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_SYSFS_PATH "/sys/class/gpio"
#define GPIO_BUF_SIZE 64

/* Export a GPIO pin to userspace */
static inline int exportPin(int pin) {
    char buf[GPIO_BUF_SIZE];
    int fd = open(GPIO_SYSFS_PATH "/export", O_WRONLY);
    if (fd < 0) {
        perror("gpio/export");
        return -1;
    }
    int len = snprintf(buf, sizeof(buf), "%d", pin);
    write(fd, buf, len);
    close(fd);
    return 0;
}

/* Unexport a GPIO pin */
static inline int unexportPin(int pin) {
    char buf[GPIO_BUF_SIZE];
    int fd = open(GPIO_SYSFS_PATH "/unexport", O_WRONLY);
    if (fd < 0) {
        perror("gpio/unexport");
        return -1;
    }
    int len = snprintf(buf, sizeof(buf), "%d", pin);
    write(fd, buf, len);
    close(fd);
    return 0;
}

/* Set GPIO direction: "in" or "out" */
static inline int setDirPin(int pin, const char *dir) {
    char path[GPIO_BUF_SIZE];
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("gpio/direction");
        return -1;
    }
    write(fd, dir, strlen(dir));
    close(fd);
    return 0;
}

/* Write value to GPIO pin: 0 or 1 */
static inline int writePin(int pin, int value) {
    char path[GPIO_BUF_SIZE];
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("gpio/value");
        return -1;
    }
    char v = value ? '1' : '0';
    write(fd, &v, 1);
    close(fd);
    return 0;
}

/* Read value from GPIO pin */
static inline int readPin(int pin) {
    char path[GPIO_BUF_SIZE];
    char val;
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/value", pin);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("gpio/value");
        return -1;
    }
    read(fd, &val, 1);
    close(fd);
    return val - '0';
}

/* Set edge trigger for interrupts: "rising", "falling", "both", "none" */
static inline int setEdge(int pin, const char *edge) {
    char path[GPIO_BUF_SIZE];
    snprintf(path, sizeof(path), GPIO_SYSFS_PATH "/gpio%d/edge", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("gpio/edge");
        return -1;
    }
    write(fd, edge, strlen(edge));
    close(fd);
    return 0;
}

#endif /* GPIO_H */
