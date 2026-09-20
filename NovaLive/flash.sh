#!/usr/bin/env bash
# Build and flash NovaLive (app + wake word model). Usage: ./flash.sh [serial-port]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"

if [ ! -f "$HERE/main/config.h" ]; then
  echo "config.h is missing. Run: cp NovaLive/main/config.example.h NovaLive/main/config.h" >&2
  exit 1
fi
if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "ESP-IDF 5.4.4+ not found at $IDF_DIR (set IDF_PATH)." >&2
  exit 1
fi
git -C "$HERE/.." submodule update --init third_party/esp-webrtc-solution

# The IDF Python env here was built with python3.14, while the default
# python3 is 3.13 - point export.sh at the env that exists instead of
# letting it look for one that doesn't.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
  env_dir=$(ls -d "$HOME"/.espressif/python_env/idf5.5_py* 2>/dev/null | tail -1 || true)
  [ -n "$env_dir" ] && export IDF_PYTHON_ENV_PATH="$env_dir"
fi
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null

PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "No board detected. Plug it in, or pass the port: ./flash.sh /dev/cu.usbmodem101" >&2
  exit 1
fi

cd "$HERE"
echo "==> building and flashing to $PORT (first build downloads components and takes a few minutes)"
# 460800 baud dropped the native USB port mid-write on this board.
idf.py -p "$PORT" -b 115200 flash
# The RTS reset idf.py ends with leaves this board in download mode
# ("waiting for download"); a watchdog reset boots the app.
esptool.py --chip esp32s3 -p "$PORT" --after watchdog_reset read_mac >/dev/null
echo "==> done. Watch it with:  bridge run -v   (or: cd NovaLive && idf.py -p $PORT monitor)"
