/*
 * UART Communication Example for LicheeRV Nano
 * Uses termios for UART1 at 115200 baud
 *
 * Pinmux setup before running:
 *   devmem 0x03001068 b 0x6   # UART1 RX
 *   devmem 0x03001064 b 0x6   # UART1 TX
 *
 * Reference: https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#define UART_DEV   "/dev/ttyS1"   /* UART1 */
#define BAUD_RATE  B115200

int main() {
    int fd;
    struct termios tio;

    printf("UART Example — Opening %s at 115200 baud\n", UART_DEV);

    fd = open(UART_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open uart");
        return 1;
    }

    /* Configure UART */
    memset(&tio, 0, sizeof(tio));
    tio.c_cflag = BAUD_RATE | CS8 | CLOCAL | CREAD;
    tio.c_iflag = IGNPAR;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 10;  /* 1 second timeout */

    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &tio);

    /* Send a test message */
    const char *msg = "Hello from LicheeRV Nano UART!\r\n";
    int n = write(fd, msg, strlen(msg));
    printf("Sent %d bytes: %s", n, msg);

    /* Read response (if any) */
    printf("Waiting for response...\n");
    char buf[256];
    for (int i = 0; i < 10; i++) {
        usleep(200000);
        int rd = read(fd, buf, sizeof(buf) - 1);
        if (rd > 0) {
            buf[rd] = '\0';
            printf("Received (%d bytes): %s\n", rd, buf);
        }
    }

    close(fd);
    printf("Done.\n");
    return 0;
}
