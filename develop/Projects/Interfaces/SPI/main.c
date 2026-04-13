/*
 * SPI Loopback Test for LicheeRV Nano
 * Uses /dev/spidev2.0
 *
 * Setup:
 *   /etc/init.d/S30wifi stop
 *   devmem 0x0300109C b 0x1   # P18 = SPI2_SDI
 *   devmem 0x030010A0 b 0x1   # P21 = SPI2_SDO
 *   devmem 0x030010A4 b 0x1   # P22 = SPI2_SCK
 *   devmem 0x030010A8 b 0x1   # P23 = SPI2_CS
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
#include <linux/spi/spidev.h>

#define SPI_DEV    "/dev/spidev2.0"
#define SPI_SPEED  1000000  /* 1 MHz */
#define SPI_BITS   8
#define SPI_MODE   SPI_MODE_0

int main() {
    int fd;
    uint8_t mode = SPI_MODE;
    uint8_t bits = SPI_BITS;
    uint32_t speed = SPI_SPEED;

    printf("SPI Test — %s\n", SPI_DEV);

    fd = open(SPI_DEV, O_RDWR);
    if (fd < 0) {
        perror("open spi");
        return 1;
    }

    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    /* Loopback test: connect MOSI to MISO */
    uint8_t tx[] = "Hello SPI!";
    uint8_t rx[sizeof(tx)] = {0};

    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = sizeof(tx),
        .speed_hz = speed,
        .bits_per_word = bits,
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 0) {
        perror("SPI_IOC_MESSAGE");
        close(fd);
        return 1;
    }

    printf("TX: %s\n", tx);
    printf("RX: ");
    for (size_t i = 0; i < sizeof(tx); i++) {
        printf("0x%02X ", rx[i]);
    }
    printf("\n");

    /* Check loopback */
    if (memcmp(tx, rx, sizeof(tx)) == 0) {
        printf("Loopback OK!\n");
    } else {
        printf("Loopback FAIL (connect MOSI→MISO for loopback test)\n");
    }

    close(fd);
    return 0;
}
