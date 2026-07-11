#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

required=(cmake make unzip wget riscv64-unknown-linux-musl-gcc riscv64-unknown-linux-musl-g++)
for command_name in "${required[@]}"; do
    command -v "$command_name" >/dev/null || {
        echo "Missing required command: $command_name" >&2
        exit 1
    }
done

test -n "${COMPILER:-}" || { echo "COMPILER is not configured" >&2; exit 1; }
test -n "${SDK_PATH:-}" || { echo "SDK_PATH is not configured" >&2; exit 1; }
test -d "$SDK_PATH/cvitek_tdl_sdk/include" || {
    echo "CVITEK TDL SDK headers were not found under $SDK_PATH" >&2
    exit 1
}

bash "$PROJECT_DIR/libs/download.sh"

echo "Inference development dependencies are ready."
echo "Run scripts/build.sh inside the container to build Yolo_CSIStream."
