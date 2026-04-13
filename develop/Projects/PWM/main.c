/*
 * PWM Control Example for LicheeRV Nano
 * Uses Linux sysfs PWM interface (/sys/class/pwm/)
 *
 * PWM Controllers: pwmchip0, pwmchip4, pwmchip8, pwmchip12
 * Relative Channel = Absolute Channel - Chip Offset
 *
 * Before running, set pinmux for PWM6 (on pwmchip4, channel 2):
 *   devmem 0x03001068 b 0x2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define PWM_CHIP     "pwmchip4"
#define PWM_CHANNEL  2   /* PWM6 → pwmchip4, channel 2 (6-4=2) */
#define PWM_PERIOD   20000000   /* 20ms = 50Hz (servo frequency) */

static void pwm_write(const char *attr, const char *value) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/pwm/%s/pwm%d/%s", PWM_CHIP, PWM_CHANNEL, attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror(path);
        return;
    }
    write(fd, value, strlen(value));
    close(fd);
}

static void pwm_export(void) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/pwm/%s/export", PWM_CHIP);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror(path);
        return;
    }
    char ch[8];
    int len = snprintf(ch, sizeof(ch), "%d", PWM_CHANNEL);
    write(fd, ch, len);
    close(fd);
    usleep(100000); /* wait for sysfs to create the channel node */
}

int main() {
    char buf[32];

    printf("PWM Sweep — %s channel %d\n", PWM_CHIP, PWM_CHANNEL);

    pwm_export();

    /* Set period */
    snprintf(buf, sizeof(buf), "%d", PWM_PERIOD);
    pwm_write("period", buf);

    /* Enable */
    pwm_write("enable", "1");

    /* Sweep duty cycle from 5% to 95% */
    for (int pct = 5; pct <= 95; pct += 5) {
        int duty = (long long)PWM_PERIOD * pct / 100;
        snprintf(buf, sizeof(buf), "%d", duty);
        pwm_write("duty_cycle", buf);
        printf("Duty: %d%% (%d ns)\n", pct, duty);
        usleep(300000);
    }

    /* Disable */
    pwm_write("enable", "0");
    printf("Done.\n");
    return 0;
}
