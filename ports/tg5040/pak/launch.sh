#!/bin/sh
set -eu

PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

BIN="${PAK_DIR}/bin/fugazi"

if [ -x "${BIN}" ]; then
  export FUGAZI_PAK_DIR="${PAK_DIR}"
  export FUGAZI_SHADER_DIR="/mnt/SDCARD/Shaders"

  LOG_DIR="/mnt/SDCARD/.userdata/tg5040/logs"
  mkdir -p "${LOG_DIR}" 2>/dev/null || true

  cd "${PAK_DIR}"
  exec "${BIN}" 2>"${LOG_DIR}/fugazi.txt"
else
  echo "Executable not found: ${BIN}"
  exit 0
fi
