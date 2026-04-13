# =============================================================
# LicheeRV Nano - RISC-V Cross-Compilation Environment
# Based on: https://medium.com/@ret7020/licheerv-nano-board-programming-part-2-a10e33b0e110
# =============================================================
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install essential build tools
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    wget \
    curl \
    git \
    tar \
    file \
    vim \
    && rm -rf /var/lib/apt/lists/*

# Create workspace
WORKDIR /workspace

# Download and extract the RISC-V musl cross-compiler toolchain (from Sophon/CVITEK)
RUN wget -q --show-progress \
    https://sophon-file.sophon.cn/sophon-prod-s3/drive/23/03/07/16/host-tools.tar.gz \
    -O /tmp/host-tools.tar.gz \
    && tar -xzf /tmp/host-tools.tar.gz -C /workspace/ \
    && rm /tmp/host-tools.tar.gz

# Apply the ioctl fix: comment out the problematic #include <bits/alltypes.h>
# This prevents compilation errors when using ioctl in user programs
RUN IOCTL_H="/workspace/host-tools/gcc/riscv64-linux-musl-x86_64/sysroot/usr/include/sys/ioctl.h" && \
    if [ -f "$IOCTL_H" ]; then \
        sed -i 's|^#include <bits/alltypes.h>|//#include <bits/alltypes.h>|' "$IOCTL_H"; \
        echo "ioctl.h fix applied successfully"; \
    else \
        echo "WARNING: ioctl.h not found at expected path"; \
    fi

# Download CVITEK TDL SDK for SG200x (LicheeRV Nano / Milk-V Duo)
RUN git clone --depth 1 https://github.com/milkv-duo/cvitek-tdl-sdk-sg200x.git /workspace/cvitek-tdl-sdk-sg200x \
    && mkdir -p /workspace/sdk/cvitek_tdl_sdk \
    && ln -sf /workspace/cvitek-tdl-sdk-sg200x/include /workspace/sdk/cvitek_tdl_sdk/include \
    && ln -sf /workspace/cvitek-tdl-sdk-sg200x/lib     /workspace/sdk/cvitek_tdl_sdk/lib \
    && ln -sf /workspace/cvitek-tdl-sdk-sg200x/sample   /workspace/sdk/cvitek_tdl_sdk/sample

# Set environment variables for the cross-compiler and SDK
ENV COMPILER=/workspace/host-tools/gcc/riscv64-linux-musl-x86_64/bin
ENV CC=${COMPILER}/riscv64-unknown-linux-musl-gcc
ENV PATH="${COMPILER}:${PATH}"
ENV SDK_PATH=/workspace/sdk

# Set working directory to projects
WORKDIR /workspace/projects

# Verify compiler is working
RUN riscv64-unknown-linux-musl-gcc --version

CMD ["/bin/bash"]
