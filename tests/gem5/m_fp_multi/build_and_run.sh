#!/bin/bash
# Build and run VFP multi-register load/store tests
#
# Usage: ./build_and_run.sh [path-to-gem5.opt]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GEM5_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GEM5_OPT="${1:-$GEM5_DIR/build/ARM/gem5.opt}"
ELF="$SCRIPT_DIR/test_vfp_multi.elf"

echo "=== Building test_vfp_multi ==="
arm-none-eabi-gcc \
    -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
    -mthumb -nostdlib \
    -T "$SCRIPT_DIR/link.ld" \
    "$SCRIPT_DIR/test_vfp_multi.S" \
    -o "$ELF"

echo "Built: $ELF"
arm-none-eabi-size "$ELF"

echo ""
echo "=== Running in gem5 ==="
"$GEM5_OPT" -re \
    --outdir="$SCRIPT_DIR/m5out" \
    "$GEM5_DIR/run_m5op_bench.py" \
    --firmware "$ELF" \
    --no-art \
    --tick-limit 1000000000000 \
    --debug-flags=Exec 2>&1 | tail -30

echo ""
echo "=== Checking result ==="
if grep -q "Stopped" "$SCRIPT_DIR/m5out/simout.txt" 2>/dev/null; then
    echo "PASS: Simulation completed normally"
elif grep -q "fatal" "$SCRIPT_DIR/m5out/simerr.txt" 2>/dev/null; then
    echo "FAIL: Simulation hit fatal error"
    tail -5 "$SCRIPT_DIR/m5out/simerr.txt"
else
    echo "UNKNOWN: Check m5out/ for details"
fi
