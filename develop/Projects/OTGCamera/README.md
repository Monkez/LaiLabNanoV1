# LicheeRV Nano YOLO Camera Streamer

![System Architecture](docs/architecture.png)
<br>
![Hardware Demonstration](docs/demonstration.png)

A high-performance C++ application for the LicheeRV Nano (SG2002/CV1800B) that streams live MJPEG video while running YOLO object detection simultaneously using the built-in Hardware TPU (TDL).

## Features

- **Dual-Channel VPSS**: Utilizes the Video Processing Subsystem to create two hardware-accelerated branches from a single camera input:
  - Branch 1: High-resolution NV12 frame → HW JPEG Encoder (VENC) → Network Stream.
  - Branch 2: Rescaled RGB planar frame → HW TPU (YOLO Inference).
- **High Performance & Low Latency**:
  - **V4L2 USERPTR Zero-Copy**: Directly captures frames into Video Buffer (VB) memory, eliminating CPU `memcpy` overhead.
  - **Decoupled I/O threads**: Dedicated YOLO thread and JPEG network broadcast thread to ensure the camera capture pipeline is never blocked.
  - **Multi-client support**: Supports up to 4 concurrent MJPEG clients without lagging the camera.
- **Hardware Integration**:
  - **Status LED (A24/GPIO 504)**: Remains ON constantly when system is running successfully.
  - **Detection LED (A23/GPIO 503)**: Automatically turns ON when YOLO detects an object, helping with headless debugging.
  - **Standalone Reset Button**: A background daemon (`reset_btn`) that safely resets the IP and reboots the board if an external button on **A27 (GPIO 507)** is held for 5 seconds using interrupt-based polling.
  - **UART Serial Output ($YOLO NMEA)**: Broadcasts bounding boxes in an NMEA-like format via UART (e.g., `/dev/ttyS0`) for easy parsing by external microcontrollers like Arduino or ESP32.
- **Python IPC Client**: Includes a Python client (`otg_camera_client.py`) using raw sockets to view the stream and overlay YOLO bounding boxes remotely over WiFi/USB-OTG.

## Prerequisites

- LicheeRV Nano board with camera module.
- Cross-compilation toolchain for RISC-V (SG2000/SG2002 SDK).
- Pre-compiled YOLO model in `.cvimodel` format (e.g., `yolov8n_coco_640.cvimodel`).

## Build Instructions

### Recommended: Docker on Windows

From the repository root:

```bat
setup.bat
build_inference.bat
```

`setup.bat` installs the web backend dependencies, builds the RISC-V development
container, downloads the CVITEK TDL SDK and prepares OpenCV Mobile. The binaries
are generated under `develop/Projects/OTGCamera/build/`.

### Build manually inside the development container

On your Linux build machine (with the SDK environment configured):

```bash
bash scripts/setup_deps.sh
bash scripts/build.sh
```

Transfer the resulting `Yolo_CSIStream` binary to your board.

## Memory and resolution limits

The LicheeRV Nano used by this project reports 128 MB RAM with no swap. The
runtime accepts camera/stream resolutions up to **1920x1080**, but decides at
startup whether a requested configuration is safe. It estimates all Video Buffer
pools, caps them at 64 MiB, and reserves at least 32 MiB of current
`MemAvailable` for the kernel, CVI runtime, model, and network buffers.

For example, 1080p with YOLO 640 needs roughly 50 MiB of VB pools and is allowed
when the board has sufficient free memory. The MJPEG path keeps only the newest
JPEG in memory (maximum 1 MiB); it drops an oversized frame rather than allocating
without limit or increasing latency.

Recommended starting point for inference:

```bash
./Yolo_CSIStream yolo11n_320.cvimodel --cam 1080x720 --stream 640x480 --yolo 320 --quality 70
```

When a camera is attached, start with 1080x720, then test 1280x720 and 1920x1080
only when the printed `VB pool estimate` passes. Record capture/stream FPS, YOLO
FPS, `Skip`, and `JpegDrop` counters before raising resolution or JPEG quality.

## Usage

Run the executable on the LicheeRV Nano:

### Normal Mode (Stream + YOLO Detection)
```bash
./Yolo_CSIStream yolov8n_coco_640.cvimodel
```

### Stream-only Mode (Lowest CPU)
Disable YOLO inference to save CPU and VPSS bandwidth:
```bash
./Yolo_CSIStream dummy --no-yolo --quality 80
```

### UART Output for ESP32/Arduino
Specify the UART device and baudrate to output NMEA-formatted YOLO detections:
```bash
./Yolo_CSIStream yolov8n_coco_640.cvimodel --uart /dev/ttyS0 --baud 115200
```
> **Note:** `/dev/ttyS0` is UART0 mapped to pins **A16 (TX)** and **A17 (RX)** on the LicheeRV Nano. Since it is also used as the default debug console, you might see kernel or login logs mixed with the UART output. You can disable kernel logs printing to the console with `echo 0 > /proc/sys/kernel/printk`.

### Physical IP Reset Button
If you lose the IP configuration, solder a push-button between **A27 (GPIO 507)** and **GND**.
Hold the button for **5 seconds**, and the daemon will automatically reset `eth0` back to `192.168.100.2` and reboot the system safely.
Note: A27 natively supports an internal pull-up resistor on boot, making it rock-solid for this operation without external resistors.

## Viewing the Stream

### Browser View
Simply open `http://<LICHEERV_IP>:8080` in any web browser to view the live MJPEG stream (video only).

### Python Client (with Bounding Boxes)
Run the provided Python client on your PC to view the stream with overlaid YOLO boxes:
```bash
cd python_client
pip install opencv-python numpy
python otg_camera_client.py
```

## UART Data Format

The UART serial data follows an easy-to-parse NMEA-like protocol:
```
$YOLO,<ts_ms>,<count>[,<cls>,<x1>,<y1>,<x2>,<y2>,<score>]*<XX>\r\n
```
- `<ts_ms>`: Timestamp
- `<count>`: Number of objects detected
- `*<XX>`: XOR checksum

View `esp32_test.h` for an example Arduino sketch to read and parse this data.

## Ethernet inference benchmark

`ethernet_inference_demo` measures a camera-free path where a PC sends frames
that are already resized to 320x320. It supports direct RGB888 planar input and
bandwidth-efficient NV12 input. The NV12 path receives into three DMA buffers,
overlaps TCP reception with VPSS NV12-to-RGB conversion and TPU inference, skips
stream/VENC work, and emits compact binary detection packets over UART.

Build and copy `build/ethernet_inference_demo` to the board, then run:

```bash
./ethernet_inference_demo /root/yolo11n_320.cvimodel \
  --input nv12 --skip-vpss --uart /dev/ttyS1 --baud 921600
```

From the PC, send synthetic moving frames at the maximum sustainable rate:

```bash
python tools/send_ethernet_frames.py 192.168.100.2 \
  --format nv12 --seconds 20 --fps 0
```

To validate detections with a real image, add `--image path/to/image.jpg`. The
wire format is a 20-byte big-endian header (`LNF1`, frame id, sender time,
payload size), followed by either 307200 RGB planar bytes or 153600 NV12 bytes.
The receiver and sender input formats must match. This program is a
benchmark/demo rather than an autostart replacement for `Yolo_CSIStream`.
