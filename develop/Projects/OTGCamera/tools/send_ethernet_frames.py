"""Send synthetic or image-backed 320x320 frames to the Ethernet demo."""

import argparse
import socket
import struct
import time


WIDTH = 320
HEIGHT = 320
PLANE_BYTES = WIDTH * HEIGHT
RGB_FRAME_BYTES = PLANE_BYTES * 3
NV12_FRAME_BYTES = PLANE_BYTES * 3 // 2
MAGIC = 0x4C4E4631  # LNF1


def make_rgb_frames(count: int = 32) -> list[bytes]:
    frames = []
    for index in range(count):
        red = bytearray([24]) * PLANE_BYTES
        green = bytearray([32]) * PLANE_BYTES
        blue = bytearray([48]) * PLANE_BYTES
        left = (index * 9) % (WIDTH - 64)
        top = (index * 5) % (HEIGHT - 64)
        for row in range(top, top + 64):
            start = row * WIDTH + left
            red[start:start + 64] = b"\xf0" * 64
            green[start:start + 64] = b"\x90" * 64
            blue[start:start + 64] = b"\x30" * 64
        frames.append(bytes(red + green + blue))
    return frames


def load_rgb_image_frame(path: str) -> bytes:
    try:
        from PIL import Image
    except ImportError as exc:
        raise SystemExit("--image requires Pillow (pip install Pillow)") from exc
    image = Image.open(path).convert("RGB").resize((WIDTH, HEIGHT))
    interleaved = image.tobytes()
    return interleaved[0::3] + interleaved[1::3] + interleaved[2::3]


def clamp_byte(value: int) -> int:
    return max(0, min(255, value))


def rgb_planar_to_nv12(frame: bytes) -> bytes:
    """Convert RGB planar to BT.601 limited-range NV12 once on the sender."""
    red = frame[:PLANE_BYTES]
    green = frame[PLANE_BYTES:2 * PLANE_BYTES]
    blue = frame[2 * PLANE_BYTES:]
    y_plane = bytearray(PLANE_BYTES)
    uv_plane = bytearray(PLANE_BYTES // 2)

    for index in range(PLANE_BYTES):
        r, g, b = red[index], green[index], blue[index]
        y_plane[index] = clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16)

    uv_index = 0
    for row in range(0, HEIGHT, 2):
        for column in range(0, WIDTH, 2):
            sum_u = 0
            sum_v = 0
            for dy in (0, 1):
                base = (row + dy) * WIDTH + column
                for dx in (0, 1):
                    index = base + dx
                    r, g, b = red[index], green[index], blue[index]
                    sum_u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
                    sum_v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
            uv_plane[uv_index] = clamp_byte((sum_u + 2) // 4)
            uv_plane[uv_index + 1] = clamp_byte((sum_v + 2) // 4)
            uv_index += 2
    return bytes(y_plane + uv_plane)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--fps", type=float, default=0.0,
                        help="Target FPS; 0 sends as fast as TCP backpressure allows")
    parser.add_argument("--image", help="Optional image to resize and repeat instead of synthetic frames")
    parser.add_argument("--format", choices=("rgb", "nv12"), default="rgb",
                        help="Payload format; must match the receiver (default: rgb)")
    args = parser.parse_args()

    rgb_frames = [load_rgb_image_frame(args.image)] if args.image else make_rgb_frames()
    frames = ([rgb_planar_to_nv12(frame) for frame in rgb_frames]
              if args.format == "nv12" else rgb_frames)
    frame_bytes = NV12_FRAME_BYTES if args.format == "nv12" else RGB_FRAME_BYTES
    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
    sock.settimeout(None)

    start = time.perf_counter()
    stats_start = start
    deadline = start + args.seconds
    frame_id = 0
    stats_frames = 0
    try:
        while time.perf_counter() < deadline:
            payload = frames[frame_id % len(frames)]
            header = struct.pack("!IIQI", MAGIC, frame_id & 0xFFFFFFFF,
                                 time.time_ns() // 1000, frame_bytes)
            sock.sendall(header)
            sock.sendall(payload)
            frame_id += 1
            stats_frames += 1

            if args.fps > 0:
                target = start + frame_id / args.fps
                delay = target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)

            now = time.perf_counter()
            if now - stats_start >= 1.0:
                elapsed = now - stats_start
                fps = stats_frames / elapsed
                mbps = fps * frame_bytes * 8 / 1_000_000
                print(f"[SEND] {fps:.2f} fps  {mbps:.2f} Mbps  total={frame_id}", flush=True)
                stats_start = now
                stats_frames = 0
    finally:
        sock.close()

    elapsed = time.perf_counter() - start
    print(f"[DONE] {frame_id / elapsed:.2f} fps over {elapsed:.2f}s ({frame_id} frames)", flush=True)


if __name__ == "__main__":
    main()
