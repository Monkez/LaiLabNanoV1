# LicheeRV Nano — Development Environment

Docker-based RISC-V cross-compilation environment for the **LicheeRV Nano** board (SG2002, dual-core C906 RISC-V + ARM Cortex-A53).

Based on: [LicheeRV Nano — Board programming (Part 2)](https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110)

## Quick Start

### Windows: automated setup

Run this from the repository root. It prepares both the Python web environment and
the RISC-V/CVITEK inference toolchain:

```bat
setup.bat
```

Build the board binaries with:

```bat
build_inference.bat
```

The output is written to `develop/Projects/OTGCamera/build/`.

### 1. Build the Docker image

```bash
cd develop
docker-compose build
```

### 2. Start the container

```bash
docker-compose up -d
```

### 3. Enter the container

```bash
docker exec -it licheerv-nano-dev bash
```

### 4. Build all projects

```bash
cd /workspace/projects
chmod +x build_all.sh
./build_all.sh
```

`build_all.sh` builds the small peripheral examples only. To build the camera and
YOLO inference application inside the container, run:

```bash
bash /workspace/projects/OTGCamera/scripts/build.sh
```

### 5. Build a single project

```bash
cd /workspace/projects/HelloWorld
make build
```

### 6. Deploy to board via SCP

```bash
# Set board IP (default: 192.168.42.1)
export LICHEERV_IP=192.168.42.1

cd /workspace/projects/HelloWorld
make deploy
```

### 7. Run directly on board

```bash
make run
```

---

## Project Structure

```
develop/
├── Dockerfile              # Cross-compilation Docker image
├── docker-compose.yml      # Container orchestration
├── README.md
└── Projects/
    ├── build_all.sh        # Build all projects at once
    ├── HelloWorld/         # Basic "Hello World" test
    │   ├── main.c
    │   └── Makefile
    ├── GPIO/               # GPIO sysfs control + blink
    │   ├── gpio.h          # Reusable GPIO library
    │   ├── main.c
    │   └── Makefile
    ├── PWM/                # PWM sweep example
    │   ├── main.c
    │   └── Makefile
    ├── Interrupts/         # GPIO interrupt polling (poll())
    │   ├── main.c
    │   └── Makefile
    └── Interfaces/
        ├── UART/           # UART1 communication (termios)
        │   ├── main.c
        │   └── Makefile
        ├── SPI/            # SPI2 loopback test
        │   ├── main.c
        │   └── Makefile
        └── I2C/            # I2C AHT20 sensor reader
            ├── main.c
            └── Makefile
```

---

## Toolchain

| Component    | Detail                                   |
|-------------|------------------------------------------|
| Toolchain   | `riscv64-linux-musl-x86_64`              |
| GCC         | `riscv64-unknown-linux-musl-gcc`         |
| Source      | [Sophon host-tools](https://sophon-file.sophon.cn/sophon-prod-s3/drive/23/03/07/16/host-tools.tar.gz) |
| ABI         | musl libc (static linking)               |

## Board Debugging

| Command                          | Purpose                        |
|----------------------------------|--------------------------------|
| `dmesg`                         | Kernel messages                |
| `cat /var/log/messages`         | TDL SDK / Camera / NPU logs   |
| `cat /proc/cvitek/vi_dbg`      | CSI camera debug               |
| `cat /proc/cvitek/vi`          | CSI camera info                |
| `cat /proc/mipi-rx`            | MIPI receiver status           |

## Pinmux Reference

Before using GPIO/PWM/UART/SPI/I2C, configure the pin multiplexer using `devmem`:

```bash
# GPIO A24 (pin 504)
devmem 0x03001060 b 0x03

# PWM6 (pwmchip4, channel 2)
devmem 0x03001068 b 0x2

# UART1
devmem 0x03001068 b 0x6   # RX
devmem 0x03001064 b 0x6   # TX

# SPI2 (stop WiFi first: /etc/init.d/S30wifi stop)
devmem 0x0300109C b 0x1   # SDI
devmem 0x030010A0 b 0x1   # SDO
devmem 0x030010A4 b 0x1   # SCK
devmem 0x030010A8 b 0x1   # CS

# I2C-1 (stop WiFi first)
devmem 0x0300109C b 0x2   # SCL
devmem 0x030010A0 b 0x2   # SDA
```
