# Quick Start Guide

## Build

```bash
# Set environment (chạy mỗi lần mở terminal mới)
export COMPILER=$HOME/host-tools/gcc/riscv64-linux-musl-x86_64/bin
export SDK_PATH=$HOME

# Build
cd ~/LicheeRVNano/Projects/OTGCamera/build
cmake ..
make
```

## Deploy to Board

```bash
# Copy binary sang board (thay IP_BOARD bằng IP của board)
scp build/Yolo_CSIStream root@<IP_BOARD>:/root/
```

## Run on Board

```bash
./Yolo_CSIStream
```

## View MJPEG Stream

Mở browser và truy cập:
```
http://<BOARD_IP>:8080
```

Hoặc dùng VLC/ffplay:
```bash
ffplay http://<BOARD_IP>:8080
```

## Output mong đợi

```
================================================
  VPSS Dual Channel + MJPEG Stream Demo
  - Channel 0: YOLO (640x640 RGB_888_PLANAR)
  - Channel 1: Stream (640x480 NV21 -> JPEG)
================================================

[INIT] Setting up Video Buffers...
[VB] Initialized with 4 pools
[INIT] Setting up VPSS...
[VPSS] Channel 0 (YOLO): 640x640 RGB_888_PLANAR
[VPSS] Channel 1 (Stream): 640x480 NV21
[VPSS] Group started with 2 channels
[INIT] Setting up VENC...
[VENC] JPEG encoder initialized (Quality: 80%)
[INIT] Setting up Camera...
[V4L2] Format: 640x480 YUYV
[V4L2] Stream started with 4 buffers

========================================
  MJPEG Stream: http://192.168.x.x:8080
========================================

[RUNNING] Demo started. Press Ctrl+C to exit.

[Stats] Capture: 30.0 FPS | YOLO: 30 | Stream: 30
...
```

## Next Steps

Sau khi demo hoạt động:

1. **Thêm YOLO inference**: Thêm `cvi_tdl.h` và gọi `CVI_TDL_YOLOV8_Detection()` với frame từ Channel 0
2. **Overlay results**: Vẽ bounding boxes lên stream output

