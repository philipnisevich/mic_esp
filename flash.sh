#!/usr/bin/env bash
# Compile and upload MicScribe. Usage: ./flash.sh [serial-port]
set -euo pipefail

# This board is an ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB *octal* PSRAM.
# PSRAM=opi is required - PSRAM=enabled is quad mode and silently yields 0 MB,
# which pushes the audio buffer into internal RAM and starves the TLS stack.
# CDCOnBoot=cdc is essential too: without it Serial maps to UART0 on GPIO43/44
# instead of the native USB port, and nothing reaches the host.
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,FlashSize=16M,CDCOnBoot=cdc"
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
