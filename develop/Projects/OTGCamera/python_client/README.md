# OTG Camera Python Client

## Overview

Python client để nhận MJPEG stream và YOLO metadata từ LicheeRV Nano.

## Architecture

```
LicheeRV Nano Board                         Python Client (PC)
┌─────────────────────┐                    ┌─────────────────────┐
│                     │   MJPEG (TCP)      │                     │
│  Port 8080 ────────────────────────────► │  Stream Thread      │
│                     │                    │       │             │
│                     │   UDP Broadcast    │       ▼             │
│  Port 8081 ────────────────────────────► │  Metadata Thread    │
│                     │                    │       │             │
└─────────────────────┘                    │       ▼             │
                                           │  Overlay & Display  │
                                           │                     │
                                           └─────────────────────┘
```

## Requirements

```bash
pip install opencv-python requests numpy
```

## Usage

### Basic Usage
```bash
python otg_camera_client.py --ip 192.168.100.2
```

### All Options
```bash
python otg_camera_client.py \
    --ip 192.168.100.2 \
    --stream-port 8080 \
    --meta-port 8081
```

### As a Module
```python
from otg_camera_client import OTGCameraClient

# Create client
client = OTGCameraClient(board_ip='192.168.100.2')

# Start receiving
client.start()

# Get frame with overlaid detections
while True:
    frame = client.get_frame_with_overlay()
    if frame is not None:
        # Process frame...
        cv2.imshow("Camera", frame)
        
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

# Stop
client.stop()
```

### Get Raw Data Separately
```python
from otg_camera_client import MJPEGStreamReceiver, YoloMetadataReceiver

# Stream only
stream = MJPEGStreamReceiver("http://192.168.100.2:8080")
stream.start()
frame = stream.get_frame()

# Metadata only  
meta = YoloMetadataReceiver(port=8081)
meta.start()
detections = meta.get_detections()  # List[Detection]

for det in detections:
    print(f"{det.class_name}: ({det.x}, {det.y}, {det.width}, {det.height}) score={det.score}")
```

## Metadata JSON Format

The board broadcasts YOLO detection results as JSON via UDP:

```json
{
  "ts": 1707234567890,  // Timestamp (ms since epoch)
  "cnt": 2,             // Number of detections
  "objs": [
    {"c": 0, "x": 100.0, "y": 50.0, "w": 80.0, "h": 120.0, "s": 0.85},
    {"c": 67, "x": 200.0, "y": 100.0, "w": 60.0, "h": 90.0, "s": 0.72}
  ]
}
```

Fields:
- `c`: class ID (COCO 80 classes)
- `x`, `y`: top-left corner
- `w`, `h`: width, height
- `s`: confidence score

## Performance Notes

- **Stream**: 25-30 FPS (independent of YOLO)
- **Metadata**: 12-16 FPS (matches YOLO inference rate)
- **Not strictly synced**: Latest metadata is used for any frame
- **Low latency**: UDP broadcast, no handshake overhead
