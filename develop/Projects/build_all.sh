#!/bin/bash
# =============================================================
# build_all.sh — Build all projects for LicheeRV Nano
# Run inside the Docker container:
#   docker exec -it licheerv-nano-dev bash
#   ./build_all.sh
# =============================================================

PROJECTS=(
    "HelloWorld"
    "GPIO"
    "PWM"
    "Interrupts"
    "Interfaces/UART"
    "Interfaces/SPI"
    "Interfaces/I2C"
)

echo "============================================"
echo " Building all LicheeRV Nano projects"
echo " Compiler: ${COMPILER}"
echo "============================================"

PASS=0
FAIL=0

for proj in "${PROJECTS[@]}"; do
    echo ""
    echo ">>> Building: $proj"
    if make -C "$proj" build 2>&1; then
        echo "    ✓ $proj OK"
        PASS=$((PASS + 1))
    else
        echo "    ✗ $proj FAILED"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "============================================"
echo " Results: $PASS passed, $FAIL failed"
echo "============================================"

exit $FAIL
