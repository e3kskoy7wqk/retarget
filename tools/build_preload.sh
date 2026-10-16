#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CUDA_PATH="${CUDA_PATH:-${CUDA_HOME:-/usr/local/cuda-12.2}}"

for header in cuda.h nvml.h; do
  if [[ ! -f "$CUDA_PATH/include/$header" ]]; then
    echo "Missing $CUDA_PATH/include/$header; set CUDA_PATH to a complete CUDA Toolkit." >&2
    exit 1
  fi
done

BUILD_OUTPUT="$(mktemp "$ROOT/.a.so.build.XXXXXX")"
trap 'rm -f "$BUILD_OUTPUT"' EXIT

gcc "$ROOT/select.cpp" \
  -std=c++17 -g \
  -L"$CUDA_PATH/extras/CUPTI/lib64" \
  -I"$CUDA_PATH/extras/CUPTI/include" \
  -I"$CUDA_PATH/include" \
  -L"$CUDA_PATH/lib64" \
  -lnvptxcompiler_static \
  -lcupti_static \
  -ldl -lrt -pthread \
  -shared -fPIC -Wall -O0 \
  -o "$BUILD_OUTPUT"
chmod 755 "$BUILD_OUTPUT"
mv -f "$BUILD_OUTPUT" "$ROOT/a.so"
