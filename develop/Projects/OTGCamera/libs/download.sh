#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_DIR="$SCRIPT_DIR/opencv-mobile-4.10.0-licheerv-nano"
ARCHIVE="$SCRIPT_DIR/opencv_lib.zip"
URL="https://github.com/nihui/opencv-mobile/releases/download/v29/opencv-mobile-4.10.0-licheerv-nano.zip"

if [[ -f "$DEST_DIR/lib/cmake/opencv4/OpenCVConfig.cmake" ]]; then
    echo "OpenCV Mobile is already installed: $DEST_DIR"
    exit 0
fi

echo "Downloading OpenCV Mobile for LicheeRV Nano..."
wget --progress=dot:giga "$URL" -O "$ARCHIVE"
unzip -q -o "$ARCHIVE" -d "$SCRIPT_DIR"
rm -f "$ARCHIVE"

test -f "$DEST_DIR/lib/cmake/opencv4/OpenCVConfig.cmake"
echo "OpenCV Mobile installed: $DEST_DIR"
