/**
 * @file reset_btn.cpp
 * @brief Reset Button Daemon for LicheeRV Nano
 * 
 * Standalone background daemon that monitors a GPIO button.
 * When held for 5 seconds, resets the board's IP to 192.168.100.2
 * and reboots the system.
 * 
 * Usage: ./reset_btn [--gpio NUM] [--hold SEC] [--ip ADDR]
 *   --gpio NUM    GPIO number for button (default: 508 = A28)
 *   --hold SEC    Hold duration in seconds (default: 5)
 *   --ip ADDR     Default IP address (default: 192.168.100.2)
 * 
 * Button wiring: Connect button between GPIO pin and GND.
 *   GPIO pin ---[BUTTON]--- GND
 *   (Active LOW: pressed = 0, released = 1)
 * 
 * Build: Part of OTGCamera CMake project (separate target)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <poll.h>

// --- Defaults ---
#define DEFAULT_BTN_GPIO    "507"     // A27 (GPIOA27 = GPIO 507)
#define DEFAULT_HOLD_SEC    5         // Hold 5s to trigger reset
#define DEFAULT_IP          "192.168.100.2"
#define DEFAULT_NETMASK     "255.255.255.0"
#define DEFAULT_IFACE       "eth0"

#define POLL_INTERVAL_MS    100       // Check button every 100ms

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/**
 * @brief Write string to sysfs file
 */
static int sysfs_write(const char* path, const char* value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, value, strlen(value));
    close(fd);
    return 0;
}

/**
 * @brief Read single char from sysfs file
 */
static int sysfs_read_char(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char c = '1';
    read(fd, &c, 1);
    close(fd);
    return c;
}

/**
 * @brief Setup GPIO as input
 */
static int gpio_init_input(const char* gpio_num) {
    char path[128];
    
    // Export GPIO
    sysfs_write("/sys/class/gpio/export", gpio_num);
    usleep(200000);  // Wait for sysfs node
    
    // Set direction to input
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/direction", gpio_num);
    if (sysfs_write(path, "in") < 0) {
        printf("[RESET_BTN] ERROR: Cannot set GPIO %s as input\n", gpio_num);
        return -1;
    }
    
    // Try to enable pull-up (may not be supported on all kernels)
    // On SG2002, some pins have internal pull-ups that can be configured
    // via pinmux registers. For simplicity, use an external pull-up resistor.
    
    printf("[RESET_BTN] GPIO %s configured as input\n", gpio_num);
    return 0;
}

/**
 * @brief Set GPIO edge for interrupt
 */
static int gpio_set_edge(const char* gpio_num, const char* edge) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/edge", gpio_num);
    if (sysfs_write(path, edge) < 0) {
        printf("[RESET_BTN] ERROR: Cannot set GPIO %s edge to %s\n", gpio_num, edge);
        return -1;
    }
    return 0;
}

/**
 * @brief Read GPIO value (returns 0 or 1, -1 on error)
 */
static int gpio_read(const char* gpio_num) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/value", gpio_num);
    int c = sysfs_read_char(path);
    if (c < 0) return -1;
    return (c == '0') ? 0 : 1;
}

/**
 * @brief Cleanup GPIO
 */
static void gpio_cleanup(const char* gpio_num) {
    sysfs_write("/sys/class/gpio/unexport", gpio_num);
}

/**
 * @brief Reset network IP to default using multiple methods
 * Tries several approaches to ensure compatibility with different rootfs configs
 */
static int reset_ip(const char* ip, const char* netmask, const char* iface) {
    char cmd[256];
    
    printf("[RESET_BTN] === Resetting IP to %s ===\n", ip);
    
    // Method 1: Write /etc/network/interfaces (Debian-style)
    {
        FILE* f = fopen("/etc/network/interfaces", "w");
        if (f) {
            fprintf(f, "auto lo\n");
            fprintf(f, "iface lo inet loopback\n\n");
            fprintf(f, "auto %s\n", iface);
            fprintf(f, "iface %s inet static\n", iface);
            fprintf(f, "    address %s\n", ip);
            fprintf(f, "    netmask %s\n", netmask);
            fclose(f);
            printf("[RESET_BTN] Wrote /etc/network/interfaces\n");
        }
    }
    
    // Method 2: Write /etc/eth0.conf or similar (some buildroot systems)
    {
        char conf_path[64];
        snprintf(conf_path, sizeof(conf_path), "/etc/%s.conf", iface);
        FILE* f = fopen(conf_path, "w");
        if (f) {
            fprintf(f, "IPADDR=%s\n", ip);
            fprintf(f, "NETMASK=%s\n", netmask);
            fclose(f);
            printf("[RESET_BTN] Wrote %s\n", conf_path);
        }
    }
    
    // Method 3: Apply immediately via ip/ifconfig (so it takes effect even before reboot)
    snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", iface);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev %s 2>/dev/null", ip, netmask, iface);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", iface);
    system(cmd);
    
    // Also try ifconfig as fallback
    snprintf(cmd, sizeof(cmd), "ifconfig %s %s netmask %s up 2>/dev/null", iface, ip, netmask);
    system(cmd);
    
    printf("[RESET_BTN] IP reset applied: %s -> %s/%s\n", iface, ip, netmask);
    return 0;
}

/**
 * @brief Perform system reboot
 */
static void do_reboot() {
    printf("[RESET_BTN] === REBOOTING SYSTEM ===\n");
    fflush(stdout);
    
    // Sync filesystems first
    sync();
    sleep(1);
    
    // Try clean reboot command first
    system("reboot");
    
    // Fallback: direct syscall
    sleep(2);
    reboot(LINUX_REBOOT_CMD_RESTART);
}

/**
 * @brief Get monotonic time in seconds
 */
static double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --gpio NUM    Button GPIO number (default: %s)\n", DEFAULT_BTN_GPIO);
    printf("  --hold SEC    Hold duration to trigger reset (default: %d)\n", DEFAULT_HOLD_SEC);
    printf("  --ip ADDR     Default IP address (default: %s)\n", DEFAULT_IP);
    printf("  --daemon      Run as background daemon\n");
    printf("  -h, --help    Show this help\n");
}

int main(int argc, char* argv[]) {
    const char* btn_gpio = DEFAULT_BTN_GPIO;
    int hold_sec = DEFAULT_HOLD_SEC;
    const char* reset_ip_addr = DEFAULT_IP;
    bool daemonize = false;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpio") == 0 && i + 1 < argc) {
            btn_gpio = argv[++i];
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            reset_ip_addr = argv[++i];
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Daemonize if requested
    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid > 0) {
            // Parent exits
            printf("[RESET_BTN] Daemon started (PID %d)\n", pid);
            return 0;
        }
        // Child continues as daemon
        setsid();
        // Redirect stdout/stderr to log file
        freopen("/tmp/reset_btn.log", "a", stdout);
        freopen("/tmp/reset_btn.log", "a", stderr);
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("[RESET_BTN] Starting: GPIO=%s, Hold=%ds, ResetIP=%s\n",
           btn_gpio, hold_sec, reset_ip_addr);
    
    // Initialize GPIO
    if (gpio_init_input(btn_gpio) < 0) {
        printf("[RESET_BTN] FATAL: GPIO init failed\n");
        return 1;
    }
    
    // Set edge to "both" for interrupt on press and release
    if (gpio_set_edge(btn_gpio, "both") < 0) {
        printf("[RESET_BTN] FATAL: GPIO edge setting failed\n");
        return 1;
    }

    char valuePath[128];
    snprintf(valuePath, sizeof(valuePath), "/sys/class/gpio/gpio%s/value", btn_gpio);
    int fd = open(valuePath, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("[RESET_BTN] ERROR: Unable to open GPIO value file");
        return 1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI; // POLLERR is implicitly polled

    // Consume any initial pending interrupt
    poll(&pfd, 1, 0);
    lseek(fd, 0, SEEK_SET);
    char tmp[4];
    read(fd, tmp, sizeof(tmp));

    // Safety check: detect floating pin at startup
    // If GPIO reads LOW immediately, the pin is likely floating (not connected)
    // or the button is stuck. Wait for it to go HIGH before monitoring.
    {
        usleep(100000);  // 100ms settle time after GPIO init
        int initial = gpio_read(btn_gpio);
        if (initial == 0) {
            printf("[RESET_BTN] WARNING: GPIO reads LOW at startup - floating pin or stuck button!\n");
            printf("[RESET_BTN] Waiting for button release before monitoring (timeout 30s)...\n");
            
            double wait_start = get_time();
            const double WAIT_TIMEOUT = 30.0;  // 30 second timeout
            
            while (g_running && gpio_read(btn_gpio) == 0) {
                double waited = get_time() - wait_start;
                if (waited >= WAIT_TIMEOUT) {
                    printf("[RESET_BTN] ERROR: GPIO still LOW after %.0fs - pin may be floating (no pull-up resistor?)\n", WAIT_TIMEOUT);
                    printf("[RESET_BTN] Aborting to prevent false trigger. Add a pull-up resistor or check wiring.\n");
                    gpio_cleanup(btn_gpio);
                    close(fd);
                    return 1;
                }
                usleep(500000);  // Check every 500ms
            }
            
            if (!g_running) {
                gpio_cleanup(btn_gpio);
                close(fd);
                return 0;
            }
            printf("[RESET_BTN] Button released, starting monitoring.\n");
            
            // Clear any event generated during release
            poll(&pfd, 1, 0);
            lseek(fd, 0, SEEK_SET);
            read(fd, tmp, sizeof(tmp));
        } else {
            printf("[RESET_BTN] GPIO reads HIGH at startup (OK - pull-up working)\n");
        }
    }
    
    // Main event loop
    double press_start = 0.0;
    bool was_pressed = false;
    
    printf("[RESET_BTN] Monitoring button interrupts (hold %ds to reset IP to %s)...\n",
           hold_sec, reset_ip_addr);
    
    while (g_running) {
        int timeout_ms = -1; // -1 means wait indefinitely
        
        if (was_pressed) {
            double elapsed = get_time() - press_start;
            double remaining = (double)hold_sec - elapsed;
            
            if (remaining <= 0.0) {
                // Done holding! Check it's still pressed to be safe
                int val = gpio_read(btn_gpio);
                if (val == 0) {
                    printf("[RESET_BTN] *** BUTTON HELD FOR %ds - RESETTING IP ***\n", hold_sec);
                    reset_ip(reset_ip_addr, DEFAULT_NETMASK, DEFAULT_IFACE);
                    sync();
                    sleep(1);
                    do_reboot();
                    return 0;
                } else {
                    was_pressed = false; // it was released exactly at timeout
                    timeout_ms = -1;
                }
            } else {
                timeout_ms = (int)(remaining * 1000.0);
            }
        }
        
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            continue; // Interrupted by signal
        }
        
        if (ret == 0) {
            // Timeout reached! This means no edge changes occurred during the 5s wait
            if (was_pressed) {
                int val = gpio_read(btn_gpio);
                if (val == 0) {
                    printf("[RESET_BTN] *** BUTTON HELD FOR %ds - RESETTING IP ***\n", hold_sec);
                    reset_ip(reset_ip_addr, DEFAULT_NETMASK, DEFAULT_IFACE);
                    sync();
                    sleep(1);
                    do_reboot();
                    return 0;
                } else {
                    was_pressed = false;
                }
            }
            continue;
        }

        if (pfd.revents & POLLPRI) {
            // Consume event so poll doesn't immediately return again
            lseek(fd, 0, SEEK_SET);
            read(fd, tmp, sizeof(tmp));
            
            int val = gpio_read(btn_gpio);
            if (val == 0) {
                // Falling Edge (Pressed)
                if (!was_pressed) {
                    was_pressed = true;
                    press_start = get_time();
                    printf("[RESET_BTN] Button pressed, hold for %ds to reset...\n", hold_sec);
                }
            } else {
                // Rising Edge (Released)
                if (was_pressed) {
                    double elapsed = get_time() - press_start;
                    printf("[RESET_BTN] Button released after %.1fs (need %ds)\n", elapsed, hold_sec);
                    was_pressed = false;
                }
            }
        }
    }
    
    close(fd);
    gpio_cleanup(btn_gpio);
    printf("[RESET_BTN] Stopped\n");
    return 0;
}
