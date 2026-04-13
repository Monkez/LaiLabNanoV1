/*
 * GPIO Blink Example for LicheeRV Nano
 * Toggles GPIO pin 504 (LED on A24) at 500ms interval
 *
 * Before running on the board, set the pinmux:
 *   devmem 0x03001060 b 0x03
 */
#include <stdio.h>
#include <unistd.h>
#include "gpio.h"

#define LED_PIN 504

int main() {
    printf("GPIO Blink — Pin %d\n", LED_PIN);

    exportPin(LED_PIN);
    setDirPin(LED_PIN, "out");

    for (int i = 0; i < 20; i++) {
        writePin(LED_PIN, 1);
        printf("ON\n");
        usleep(500000);  /* 500ms */

        writePin(LED_PIN, 0);
        printf("OFF\n");
        usleep(500000);
    }

    unexportPin(LED_PIN);
    printf("Done.\n");
    return 0;
}
