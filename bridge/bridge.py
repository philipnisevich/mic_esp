#!/usr/bin/env python3
"""Host-side bridge for MicScribe.

The ESP32 does all the recording and transcribing on its own; this script just
listens on the serial port and does something useful with the transcripts it
emits (print them, copy them to the clipboard, or type them into whatever
window has focus).

    pip install pyserial
    ./bridge.py                     # auto-detect the board, print transcripts
    ./bridge.py --copy              # also copy each transcript to the clipboard
    ./bridge.py --type              # also type it into the focused app (macOS)
    ./bridge.py --port /dev/cu.usbmodem101 --verbose
"""

import argparse
import json
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing. Install it with:  pip install pyserial")

BAUD = 115200


def find_port():
    candidates = []
    for p in list_ports.comports():
        name = p.device
        if "Bluetooth" in name or "debug-console" in name:
            continue
        if any(tag in name for tag in ("usbmodem", "usbserial", "wchusbserial", "SLAB")):
            candidates.append(name)
    if not candidates:
        return None
    return sorted(candidates)[0]


def copy_to_clipboard(text):
    if sys.platform == "darwin":
        cmd = ["pbcopy"]
    elif sys.platform.startswith("linux"):
        cmd = ["xclip", "-selection", "clipboard"]
    else:
        cmd = ["clip"]
    try:
        subprocess.run(cmd, input=text.encode("utf-8"), check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"[bridge] clipboard failed: {exc}", file=sys.stderr)


def type_text(text):
    """Type the transcript into the frontmost app (macOS only)."""
    if sys.platform != "darwin":
        print("[bridge] --type is macOS-only", file=sys.stderr)
        return
    # A literal newline inside an AppleScript string literal is a syntax error,
    # so splice line breaks in as `return` instead of embedding them.
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    escaped = escaped.replace("\r\n", "\n").replace("\r", "\n")
    escaped = escaped.replace("\n", '" & return & "')
    script = f'tell application "System Events" to keystroke "{escaped}"'
    try:
        subprocess.run(["osascript", "-e", script], check=True)
    except subprocess.CalledProcessError:
        print(
            "[bridge] osascript refused - grant your terminal Accessibility "
            "permission in System Settings > Privacy & Security",
            file=sys.stderr,
        )


def reset_board(ser):
    """Reboot the ESP32 the same way the Arduino IDE monitor does.

    EN is driven by RTS and GPIO0 by DTR, so holding EN low briefly with GPIO0
    high restarts the board into normal run mode.
    """
    try:
        # The ESP32-S3's built-in USB Serial/JTAG has no physical DTR/RTS lines;
        # it decodes a specific sequence of the two into a reset instead. This
        # is the order esptool uses, and the classic EN/GPIO0 wiggle does
        # nothing on these parts.
        ser.setRTS(False)
        ser.setDTR(False)
        time.sleep(0.1)
        ser.setDTR(True)
        ser.setRTS(False)
        time.sleep(0.1)
        ser.setRTS(True)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.05)
        ser.reset_input_buffer()
    except (OSError, serial.SerialException) as exc:
        print(f"[bridge] reset failed: {exc}", file=sys.stderr)


def handle_line(line, args):
    line = line.strip()
    if not line:
        return

    if line.startswith("#"):
        if args.verbose:
            print(f"\033[90m{line}\033[0m")
        return

    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        if args.verbose:
            print(f"\033[90m{line}\033[0m")
        return

    kind = event.get("type")
    if kind == "transcript":
        text = event.get("text", "")
        lang = event.get("language")
        tag = f" [{lang}]" if lang and args.verbose else ""
        print(f"\033[92m>{tag}\033[0m {text}")
        if args.copy:
            copy_to_clipboard(text)
        if args.type:
            type_text(text)
    elif kind == "answer":
        # The model's reply. Printed, not typed - --type stays bound to
        # transcripts so dictation keeps working as before.
        mark = "\u2726" if not event.get("research") else "\u2295"
        print(f"\033[96m{mark}\033[0m {event.get('text', '')}")
        if args.verbose and event.get("model"):
            print(f"\033[90m   via {event['model']}\033[0m")
    elif kind == "noise":
        # Non-speech: a false wake, or a stray tap. Never type or copy this.
        if args.verbose:
            detail = event.get("text") or event.get("reason") or ""
            print(f"\033[90m· noise discarded {detail}\033[0m")
    elif kind == "error":
        print(f"\033[91m! {event.get('error')}\033[0m", file=sys.stderr)
    elif kind == "state" and args.verbose:
        print(f"\033[94m· {event.get('state')}\033[0m")


def main():
    ap = argparse.ArgumentParser(description="Serial bridge for MicScribe")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--copy", action="store_true", help="copy transcripts to the clipboard")
    ap.add_argument("--type", action="store_true", help="type transcripts into the focused app")
    ap.add_argument("--verbose", "-v", action="store_true", help="show device log lines")
    ap.add_argument(
        "--reset", action="store_true",
        help="pulse DTR/RTS on connect to reboot the board (shows its boot log)"
    )
    args = ap.parse_args()

    # Without this, Python block-buffers stdout whenever it is not a terminal,
    # so transcripts vanish into a pipe or a log file until the buffer fills.
    sys.stdout.reconfigure(line_buffering=True)

    port = args.port or find_port()
    if not port:
        sys.exit("No serial port found. Pass one with --port.")

    print(f"[bridge] listening on {port} @ {args.baud}")

    while True:
        try:
            with serial.Serial(port, args.baud, timeout=1) as ser:
                # The S3's USB-Serial/JTAG gates its TX on the host looking
                # "connected". Opening a /dev/cu.* device does not necessarily
                # assert DTR, and without it the firmware's output is discarded
                # before it ever reaches us.
                try:
                    ser.dtr = True
                    ser.rts = False
                except (OSError, serial.SerialException):
                    pass
                if args.reset:
                    reset_board(ser)
                while True:
                    raw = ser.readline()
                    if raw:
                        handle_line(raw.decode("utf-8", errors="replace"), args)
        except serial.SerialException as exc:
            print(f"[bridge] {exc} - retrying in 2s", file=sys.stderr)
            time.sleep(2)
        except KeyboardInterrupt:
            print("\n[bridge] bye")
            return


if __name__ == "__main__":
    main()
