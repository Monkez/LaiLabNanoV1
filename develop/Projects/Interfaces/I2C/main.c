/*
 * I2C AHT20 Sensor Example for LicheeRV Nano
 * Reads temperature & humidity from AHT20 sensor on I2C-1
 *
 * Setup:
 *   /etc/init.d/S30wifi stop
 *   devmem 0x0300109C b 0x2   # P18 = I2C1_SCL
 *   devmem 0x030010A0 b 0x2   # P21 = I2C1_SDA
 *
 * Verify device: i2cdetect -y 1  (should show 0x38)
 *
 * Reference: https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define I2C_DEV    "/dev/i2c-1"
#define AHT20_ADDR 0x38

/* AHT20 Commands */
#define AHT20_CMD_INIT     0xBE
#define AHT20_CMD_MEASURE  0xAC
#define AHT20_CMD_SOFTRST  0xBA

static int i2c_fd;

static int aht20_init(void) {
    uint8_t cmd[] = {AHT20_CMD_INIT, 0x08, 0x00};
    if (write(i2c_fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
        perror("aht20 init");
        return -1;
    }
    usleep(40000);
    return 0;
}

static int aht20_read(float *temperature, float *humidity) {
    uint8_t cmd[] = {AHT20_CMD_MEASURE, 0x33, 0x00};
    if (write(i2c_fd, cmd, sizeof(cmd)) != sizeof(cmd)) {
        perror("aht20 measure");
        return -1;
    }
    usleep(80000); /* 80ms measurement time */

    uint8_t data[7];
    if (read(i2c_fd, data, sizeof(data)) != sizeof(data)) {
        perror("aht20 read");
        return -1;
    }

    /* Check busy bit */
    if (data[0] & 0x80) {
        printf("Sensor busy, retrying...\n");
        usleep(100000);
        if (read(i2c_fd, data, sizeof(data)) != sizeof(data)) return -1;
    }

    /* Parse humidity (20-bit) */
    uint32_t raw_hum = ((uint32_t)(data[1]) << 12) |
                       ((uint32_t)(data[2]) << 4) |
                       ((uint32_t)(data[3]) >> 4);
    *humidity = (float)raw_hum / 1048576.0f * 100.0f;

    /* Parse temperature (20-bit) */
    uint32_t raw_temp = (((uint32_t)(data[3]) & 0x0F) << 16) |
                        ((uint32_t)(data[4]) << 8) |
                        (uint32_t)(data[5]);
    *temperature = (float)raw_temp / 1048576.0f * 200.0f - 50.0f;

    return 0;
}

int main() {
    printf("I2C AHT20 — %s @ 0x%02X\n", I2C_DEV, AHT20_ADDR);

    i2c_fd = open(I2C_DEV, O_RDWR);
    if (i2c_fd < 0) {
        perror("open i2c");
        return 1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, AHT20_ADDR) < 0) {
        perror("ioctl I2C_SLAVE");
        close(i2c_fd);
        return 1;
    }

    aht20_init();

    /* Read 10 samples */
    for (int i = 0; i < 10; i++) {
        float temp, hum;
        if (aht20_read(&temp, &hum) == 0) {
            printf("[%2d] Temperature: %.1f°C  Humidity: %.1f%%\n", i + 1, temp, hum);
        }
        sleep(2);
    }

    close(i2c_fd);
    printf("Done.\n");
    return 0;
}
