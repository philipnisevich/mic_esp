#!/usr/bin/env bash
# Compile and upload MicScribe. Usage: ./flash.sh [serial-port]
set -euo pipefail

FQBN="esp32:esp32:esp32s3:PSRAM=enabled,PartitionScheme=huge_app,FlashSize=4M"
SKETCH="$(cd "$(dirname "$0")" && pwd)/MicScribe"

if [ ! -f "$SKETCH/config.h" ]; then
  echo "config.h is missing. Run: cp MicScribe/config.example.h MicScribe/config.h" >&2
  exit 1
fi

PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT=$(arduino-cli board list | awk '/usbmodem|usbserial|wchusbserial|SLAB/ {print $1; exit}')
fi
if [ -z "$PORT" ]; then
  echo "No board detected. Plug it in, or pass the port: ./flash.sh /dev/cu.usbmodem101" >&2
  exit 1
fi

echo "==> compiling"
arduino-cli compile --fqbn "$FQBN" "$SKETCH"

echo "==> uploading to $PORT"
arduino-cli upload --fqbn "$FQBN" --port "$PORT" "$SKETCH"

echo "==> done. Watch it with:  arduino-cli monitor -p $PORT -c baudrate=115200"
