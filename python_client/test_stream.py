#!/usr/bin/env python3
"""
LicheeRV Nano OTG Camera Client

Receives MJPEG stream and YOLO metadata from LicheeRV Nano board.
Draws bounding boxes overlay on the stream.

Usage:
    python otg_camera_client.py --ip 192.168.100.2

Requirements:
    pip install opencv-python numpy
"""

import argparse
import json
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import List, Optional

import cv2
import numpy as np

# COCO class names for display
COCO_CLASSES = [
    'person', 'bicycle', 'car', 'motorcycle', 'airplane', 'bus', 'train', 'truck', 'boat',
    'traffic light', 'fire hydrant', 'stop sign', 'parking meter', 'bench', 'bird', 'cat',
    'dog', 'horse', 'sheep', 'cow', 'elephant', 'bear', 'zebra', 'giraffe', 'backpack',
    'umbrella', 'handbag', 'tie', 'suitcase', 'frisbee', 'skis', 'snowboard', 'sports ball',
    'kite', 'baseball bat', 'baseball glove', 'skateboard', 'surfboard', 'tennis racket',
    'bottle', 'wine glass', 'cup', 'fork', 'knife', 'spoon', 'bowl', 'banana', 'apple',
    'sandwich', 'orange', 'broccoli', 'carrot', 'hot dog', 'pizza', 'donut', 'cake', 'chair',
    'couch', 'potted plant', 'bed', 'dining table', 'toilet', 'tv', 'laptop', 'mouse',
    'remote', 'keyboard', 'cell phone', 'microwave', 'oven', 'toaster', 'sink', 'refrigerator',
    'book', 'clock', 'vase', 'scissors', 'teddy bear', 'hair drier', 'toothbrush'
]

# Colors for bounding boxes (BGR)
COLORS = [
    (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255),
    (255, 0, 255), (128, 255, 0), (255, 128, 0), (0, 128, 255), (128, 0, 255)
]


@dataclass
class Detection:
    """Single detection result"""
    class_id: int
    x: float
    y: float
    width: float
    height: float
    score: float
    
    @property
    def class_name(self) -> str:
        if 0 <= self.class_id < len(COCO_CLASSES):
            return COCO_CLASSES[self.class_id]
        return f"class_{self.class_id}"


class YoloMetadataReceiver:
    """Receives YOLO detection metadata via UDP broadcast"""
    
    def __init__(self, port: int = 8081):
        self.port = port
        self.running = False
        self.thread: Optional[threading.Thread] = None
        self.latest_detections: List[Detection] = []
        self.latest_timestamp: int = 0
        self.yolo_w: int = 640  # Updated from metadata
        self.yolo_h: int = 640
        self.stream_w: int = 640
        self.stream_h: int = 480
        self.lock = threading.Lock()
        self.stats = {'received': 0, 'fps': 0.0, 'last_time': time.time()}
        
    def start(self):
        """Start receiving metadata in background thread"""
        self.running = True
        self.thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.thread.start()
        print(f"[META] Started UDP receiver on port {self.port}")
        
    def stop(self):
        """Stop receiving"""
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
            
    def _receive_loop(self):
        """Background thread for UDP reception"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('', self.port))
        sock.settimeout(0.5)
        
        last_fps_time = time.time()
        frame_count = 0
        
        while self.running:
            try:
                data, addr = sock.recvfrom(4096)
                self._parse_metadata(data.decode('utf-8'))
                frame_count += 1
                
                # Calculate FPS every second
                now = time.time()
                if now - last_fps_time >= 1.0:
                    self.stats['fps'] = frame_count / (now - last_fps_time)
                    self.stats['received'] += frame_count
                    frame_count = 0
                    last_fps_time = now
                    
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[META] Error: {e}")
                
        sock.close()
        
    def _parse_metadata(self, json_str: str):
        """Parse JSON metadata from board (includes resolution info)"""
        try:
            data = json.loads(json_str)
            detections = []
            
            for obj in data.get('objs', []):
                det = Detection(
                    class_id=obj['c'],
                    x=obj['x'],
                    y=obj['y'],
                    width=obj['w'],
                    height=obj['h'],
                    score=obj['s']
                )
                detections.append(det)
                
            with self.lock:
                self.latest_detections = detections
                self.latest_timestamp = data.get('ts', 0)
                # Update resolutions from metadata (if present)
                if 'yw' in data:
                    self.yolo_w = data['yw']
                    self.yolo_h = data['yh']
                if 'sw' in data:
                    self.stream_w = data['sw']
                    self.stream_h = data['sh']
                
        except json.JSONDecodeError as e:
            print(f"[META] JSON parse error: {e}")
            
    def get_detections(self) -> List[Detection]:
        """Get latest detections (thread-safe)"""
        with self.lock:
            return list(self.latest_detections)
    
    def get_resolutions(self) -> tuple:
        """Get YOLO and stream resolutions (thread-safe)"""
        with self.lock:
            return self.yolo_w, self.yolo_h, self.stream_w, self.stream_h


class MJPEGStreamReceiver:
    """
    Receives MJPEG stream via raw HTTP socket.
    
    Uses raw socket instead of `requests` library for better control over
    timeouts and buffering. This avoids the common issues with requests'
    connection pooling and read timeout handling for long-lived streams.
    """
    
    def __init__(self, host: str, port: int = 8080):
        self.host = host
        self.port = port
        self.running = False
        self.thread: Optional[threading.Thread] = None
        self.latest_frame: Optional[np.ndarray] = None
        self.lock = threading.Lock()
        self.stats = {'received': 0, 'fps': 0.0}
        self._reconnect_count = 0
        
    def start(self):
        """Start receiving stream in background thread"""
        self.running = True
        self.thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.thread.start()
        print(f"[STREAM] Started MJPEG receiver from {self.host}:{self.port}")
        
    def stop(self):
        """Stop receiving"""
        self.running = False
        if self.thread:
            self.thread.join(timeout=2.0)
    
    def _connect(self) -> Optional[socket.socket]:
        """Create raw TCP connection and send HTTP GET request"""
        t0 = time.time()
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1.5)  # 1.5s connect timeout (board is on local USB network)
            sock.connect((self.host, self.port))
            t1 = time.time()
            
            # Send HTTP GET request
            request = (
                f"GET / HTTP/1.0\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                f"Accept: multipart/x-mixed-replace\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            )
            sock.sendall(request.encode())
            
            # Set longer read timeout for streaming (30s between frames max)
            sock.settimeout(30.0)
            
            # Read HTTP response header
            header_buf = b''
            while b'\r\n\r\n' not in header_buf:
                chunk = sock.recv(1024)
                if not chunk:
                    sock.close()
                    return None
                header_buf += chunk
            t2 = time.time()
            
            # Check for 200 OK
            if b'200' not in header_buf.split(b'\r\n')[0]:
                print(f"[STREAM] Bad HTTP response: {header_buf[:100]}")
                sock.close()
                return None
            
            print(f"[STREAM] Connected (connect={t1-t0:.1f}s, headers={t2-t1:.1f}s)")
            
            # Return remaining data after headers + the socket
            remaining = header_buf.split(b'\r\n\r\n', 1)[1]
            self._initial_data = remaining
            return sock
            
        except (socket.timeout, ConnectionRefusedError, OSError) as e:
            elapsed = time.time() - t0
            if self._reconnect_count < 3:
                print(f"[STREAM] Connect failed ({elapsed:.1f}s): {type(e).__name__}")
            return None

    def _receive_loop(self):
        """Background thread: connect and parse MJPEG multipart stream"""
        last_fps_time = time.time()
        frame_count = 0
        
        while self.running:
            sock = self._connect()
            if sock is None:
                self._reconnect_count += 1
                # Progressive retry: 0ms, 100ms, 200ms, 300ms, cap at 500ms
                delay = min(self._reconnect_count * 0.1, 0.5)
                if delay > 0:
                    time.sleep(delay)
                continue
            
            self._reconnect_count = 0
            buf = bytearray(self._initial_data) if self._initial_data else bytearray()
            
            try:
                while self.running:
                    # Read data from socket
                    try:
                        chunk = sock.recv(65536)  # Large recv for efficiency
                    except socket.timeout:
                        print("[STREAM] Read timeout, reconnecting...")
                        break
                    
                    if not chunk:
                        print("[STREAM] Connection closed by server, reconnecting...")
                        break
                    
                    buf.extend(chunk)
                    
                    # Parse JPEG frames from multipart stream
                    # Look for JPEG start (FFD8) and end (FFD9) markers
                    while True:
                        start = buf.find(b'\xff\xd8')
                        if start == -1:
                            # No JPEG start found, keep only last 2 bytes
                            # (in case FFD8 is split across chunks)
                            if len(buf) > 2:
                                del buf[:len(buf) - 2]
                            break
                        
                        end = buf.find(b'\xff\xd9', start + 2)
                        if end == -1:
                            # JPEG end not yet received, discard data before start
                            if start > 0:
                                del buf[:start]
                            break
                        
                        # Extract complete JPEG
                        jpg_bytes = bytes(buf[start:end + 2])
                        del buf[:end + 2]
                        
                        # Decode JPEG
                        frame = cv2.imdecode(
                            np.frombuffer(jpg_bytes, dtype=np.uint8),
                            cv2.IMREAD_COLOR
                        )
                        
                        if frame is not None:
                            with self.lock:
                                self.latest_frame = frame
                            frame_count += 1
                    
                    # Prevent buffer from growing unbounded
                    if len(buf) > 2 * 1024 * 1024:  # 2MB safety limit
                        print("[STREAM] Buffer overflow, resetting")
                        buf.clear()
                    
                    # Calculate FPS
                    now = time.time()
                    if now - last_fps_time >= 1.0:
                        self.stats['fps'] = frame_count / (now - last_fps_time)
                        self.stats['received'] += frame_count
                        frame_count = 0
                        last_fps_time = now
                        
            except Exception as e:
                print(f"[STREAM] Error: {e}")
            finally:
                try:
                    sock.close()
                except:
                    pass
            
            if self.running:
                time.sleep(0.3)  # Brief pause before reconnect
                
    def get_frame(self) -> Optional[np.ndarray]:
        """Get latest frame (thread-safe)"""
        with self.lock:
            if self.latest_frame is not None:
                return self.latest_frame.copy()
        return None


class OTGCameraClient:
    """
    Complete client for LicheeRV Nano OTG Camera
    
    Receives both MJPEG stream and YOLO metadata, then overlays bounding boxes.
    """
    
    def __init__(self, board_ip: str, stream_port: int = 8080, metadata_port: int = 8081):
        self.board_ip = board_ip
        self.stream_url = f"http://{board_ip}:{stream_port}"
        
        # Initialize receivers
        self.stream_receiver = MJPEGStreamReceiver(board_ip, stream_port)
        self.metadata_receiver = YoloMetadataReceiver(metadata_port)
        
        # Run status
        self.running = False
        
    def start(self):
        """Start both receivers"""
        self.stream_receiver.start()
        self.metadata_receiver.start()
        self.running = True
        
    def stop(self):
        """Stop both receivers"""
        self.running = False
        self.stream_receiver.stop()
        self.metadata_receiver.stop()
        
    def get_frame_with_overlay(self) -> Optional[np.ndarray]:
        """
        Get latest frame with YOLO detections overlaid
        
        Returns None if no frame available yet
        """
        frame = self.stream_receiver.get_frame()
        if frame is None:
            return None
            
        detections = self.metadata_receiver.get_detections()
        
        # Get frame dimensions
        frame_h, frame_w = frame.shape[:2]
        
        # Get YOLO model resolution from metadata (dynamic, not hardcoded)
        yolo_w, yolo_h, _, _ = self.metadata_receiver.get_resolutions()
        
        # Calculate scaling params used by YOLO (letterbox with aspect ratio)
        # VPSS uses ASPECT_RATIO_AUTO which preserves aspect ratio with padding
        scale = min(yolo_w / frame_w, yolo_h / frame_h)
        new_w = int(frame_w * scale)
        new_h = int(frame_h * scale)
        pad_x = (yolo_w - new_w) / 2
        pad_y = (yolo_h - new_h) / 2
        
        # Draw detections
        for i, det in enumerate(detections):
            color = COLORS[det.class_id % len(COLORS)]
            
            # Map YOLO coordinates (model_size x model_size) back to Stream (frame_w x frame_h)
            # 1. Remove padding
            # 2. Scale back
            x1 = int((det.x - pad_x) / scale)
            y1 = int((det.y - pad_y) / scale)
            x2 = int((det.x + det.width - pad_x) / scale)
            y2 = int((det.y + det.height - pad_y) / scale)

            # Clamp to frame bounds
            x1 = max(0, min(x1, frame_w - 1))
            y1 = max(0, min(y1, frame_h - 1))
            x2 = max(0, min(x2, frame_w - 1))
            y2 = max(0, min(y2, frame_h - 1))
            
            # Skip if box is invalid (outside visible area)
            if x2 <= x1 or y2 <= y1:
                continue
            
            # Draw bounding box
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
            
            # Draw label with background
            label = f"{det.class_name}: {det.score:.2f}"
            (label_w, label_h), baseline = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
            label_y = max(y1 - 5, label_h + 5)  # Ensure label is visible
            cv2.rectangle(frame, (x1, label_y - label_h - 5), (x1 + label_w, label_y), color, -1)
            cv2.putText(frame, label, (x1, label_y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
            
        # Draw stats overlay
        stats_text = f"Stream: {self.stream_receiver.stats['fps']:.1f} FPS | Yolo: {self.metadata_receiver.stats['fps']:.1f} FPS"
        cv2.putText(frame, stats_text, (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
        
        return frame
        
    def run_display(self, window_name: str = "OTG Camera"):
        """Run interactive display with OpenCV"""
        print(f"\n{'='*50}")
        print(f"  LicheeRV Nano OTG Camera Client")
        print(f"  Stream: {self.stream_url}")
        print(f"  Metadata Port: {self.metadata_receiver.port}")
        print(f"{'='*50}")
        print("\nPress 'q' to quit\n")
        
        self.start()
        
        try:
            while True:
                frame = self.get_frame_with_overlay()
                
                if frame is not None:
                    cv2.imshow(window_name, frame)
                    
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    break
                    
        except KeyboardInterrupt:
            print("\nInterrupted")
            
        finally:
            self.stop()
            cv2.destroyAllWindows()


def main():
    parser = argparse.ArgumentParser(description='LicheeRV Nano OTG Camera Client')
    parser.add_argument('--ip', type=str, default='192.168.100.2',
                       help='Board IP address')
    parser.add_argument('--stream-port', type=int, default=8080,
                       help='MJPEG stream port')
    parser.add_argument('--meta-port', type=int, default=8081,
                       help='Metadata UDP port')
    
    args = parser.parse_args()
    
    client = OTGCameraClient(
        board_ip=args.ip,
        stream_port=args.stream_port,
        metadata_port=args.meta_port
    )
    
    client.run_display()


if __name__ == '__main__':
    main()
