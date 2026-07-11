#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"

bash "$PROJECT_DIR/scripts/setup_deps.sh"
# Do not force a generator: an existing build directory may already use Makefiles.
# CMake reuses that generator; a new directory uses the platform default.
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "Build completed:"
file "$BUILD_DIR/Yolo_CSIStream"
file "$BUILD_DIR/reset_btn"
