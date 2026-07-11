# VPSS Dual Channel + YOLO + MJPEG Streaming

## Overview

Chương trình sử dụng **VPSS + VENC + TDL (YOLO NPU)** để xử lý video với 2 output channels:

```
USB Camera (YUYV 640x480)
        │
        ▼
   ┌─────────────────────────────────────────┐
   │              VPSS Group 0               │
   │   Input: YUYV 640x480                   │
   │   ┌──────────────┐  ┌──────────────┐    │
   │   │  Channel 0   │  │  Channel 1   │    │
   │   │  YOLO        │  │  Stream      │    │
   │   │  640x640     │  │  640x480     │    │
   │   │  RGB_PLANAR  │  │  NV12        │    │
   │   └──────────────┘  └──────────────┘    │
   └─────────────────────────────────────────┘
        │                     │
        ▼                     ▼
   YOLO Detection         VENC JPEG
   (NPU Hardware)         (Hardware)
        │                     │
        ▼                     ▼
   Print Results          HTTP Stream
   to Console             Port 8080
```

## Hardware Acceleration

| Component | Function | Acceleration |
|-----------|----------|--------------|
| VPSS | Color convert + Resize | Hardware |
| VENC | JPEG encoding | Hardware |
| TDL/NPU | YOLOv8 inference | Hardware |

## Features

1. **Dual Channel Output**: 1 input, 2 parallel outputs
2. **YOLO Detection**: Real-time object detection với FPS tracking
3. **MJPEG Streaming**: View trực tiếp qua browser
4. **All Hardware**: VPSS + VENC + NPU - minimal CPU usage

## Usage

### Build
```bash
export COMPILER=$HOME/host-tools/gcc/riscv64-linux-musl-x86_64/bin
export SDK_PATH=$HOME
cd ~/LicheeRVNano/Projects/OTGCamera/build
cmake .. && make
```

### Run
```bash
./Yolo_CSIStream yolov8n_coco_640.cvimodel
```

### View Stream
Mở browser: `http://<BOARD_IP>:8080`

## Output Format

```
================================================
  VPSS Dual Channel + YOLO + MJPEG Stream
  - Channel 0: YOLO (640x640 RGB_888_PLANAR)
  - Channel 1: Stream (640x480 NV12 -> JPEG)
  - Model: yolov8n_coco_640.cvimodel
================================================

[INIT] Setting up Video Buffers...
[VB] Initialized with 4 pools
[INIT] Setting up VPSS...
[VPSS] Channel 0 (YOLO): 640x640 RGB_888_PLANAR
[VPSS] Channel 1 (Stream): 640x480 NV12
[VPSS] Group started with 2 channels
[INIT] Setting up VENC...
[VENC] JPEG encoder initialized (Quality: 80%)
[INIT] Setting up TDL (YOLO)...
[TDL] YOLO model loaded: yolov8n_coco_640.cvimodel
[INIT] Setting up Camera...
[V4L2] Format: 640x480 YUYV
[V4L2] Stream started with 4 buffers

========================================
  MJPEG Stream: http://192.168.x.x:8080
========================================

[RUNNING] Demo started. Press Ctrl+C to exit.

[YOLO] Detected 2 objects: Class0(0.85) Class67(0.72) 
[Stats] Capture: 25.0 | YOLO: 14.0 FPS | Stream: 25 | Objects: 28
```

## YOLO Parameters

```cpp
#define MODEL_SCALE 1.0       // Preprocessing scale
#define MODEL_MEAN 0.0        // Preprocessing mean  
#define MODEL_CLASS_CNT 80    // COCO classes
#define MODEL_THRESH 0.5      // Detection threshold
#define MODEL_NMS_THRESH 0.5  // NMS threshold
```

## Performance Notes

- **YOLO FPS**: Typically 12-16 FPS với YOLOv8n (640x640)
- **Stream FPS**: 25-30 FPS (không phụ thuộc YOLO)
- **2 luồng độc lập**: Stream không bị chậm bởi YOLO
