#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="tg5040"
DOCKER_IMAGE="ghcr.io/loveretro/tg5040-toolchain"

echo ""
echo "=== Building Fugazi for ${PLATFORM} (Docker) ==="
echo ""

docker run --rm \
  -v "$PROJECT_DIR":/workspace \
  "$DOCKER_IMAGE" \
  make -C /workspace/ports/$PLATFORM

echo ""
echo "=== Build complete ==="
echo ""
