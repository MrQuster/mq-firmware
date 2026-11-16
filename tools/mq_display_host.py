#!/usr/bin/env python3
"""
MQ Secondary Display Host for ESP32-2432S028R (CYD).
Streams PC desktop/monitor frames as JPEG over USB-UART (921600 baud).
Supports --daemon mode so tapping "Display Mode" on the CYD starts streaming automatically.
"""

import argparse
import glob
import io
import os
import shutil
import struct
import subprocess
import sys
import time

import serial
from PIL import Image, ImageGrab


MAGIC = b"MQF1"
PKT_JPEG = 0x01
PKT_PING = 0x02


def find_serial_port() -> str | None:
    ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def capture_frame(output: str | None, region: str | None, use_grim: bool) -> Image.Image:
    if use_grim:
        cmd = ["grim", "-t", "ppm"]
        if output:
            cmd.extend(["-o", output])
        if region:
            cmd.extend(["-g", region])
        cmd.append("-")
        raw = subprocess.check_output(cmd, stderr=subprocess.DEVNULL)
        return Image.open(io.BytesIO(raw)).convert("RGB")
    return ImageGrab.grab().convert("RGB")


def send_jpeg_frame(ser: serial.Serial, img: Image.Image, width: int, height: int, quality: int) -> int:
    resized = img.resize((width, height), Image.Resampling.BILINEAR)
    buf = io.BytesIO()
    resized.save(buf, format="JPEG", quality=quality, subsampling=2, optimize=False)
    jpg = buf.getvalue()
    if len(jpg) > 32768:
        buf = io.BytesIO()
        resized.save(buf, format="JPEG", quality=max(30, quality - 20), subsampling=2)
        jpg = buf.getvalue()
    pkt = MAGIC + struct.pack("<BHHI", PKT_JPEG, 0, 0, len(jpg)) + jpg
    ser.write(pkt)
    return len(jpg)


def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def run_daemon(args: argparse.Namespace, use_grim: bool) -> int:
    frame_interval = 1.0 / max(1.0, args.fps)
    while True:
        port = args.port or find_serial_port()
        if not port:
            time.sleep(1.0)
            continue

        try:
            ser = open_serial(port, args.baud)
        except Exception:
            time.sleep(1.0)
            continue

        streaming = False
        width, height = args.width, args.height

        try:
            while True:
                while ser.in_waiting:
                    raw_line = ser.readline().decode("utf-8", errors="ignore").strip()
                    if raw_line.startswith("MQDISP_READY"):
                        parts = raw_line.split()
                        if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
                            width, height = int(parts[1]), int(parts[2])
                        if not streaming:
                            print(f"[MQ-Daemon] CYD Display Mode active ({width}x{height}) on {port}", flush=True)
                        streaming = True
                    elif raw_line.startswith("MQDISP_STOP"):
                        if streaming:
                            print("[MQ-Daemon] CYD exited Display Mode, idling.", flush=True)
                        streaming = False

                if streaming:
                    t0 = time.monotonic()
                    img = capture_frame(args.output, args.region, use_grim)
                    send_jpeg_frame(ser, img, width, height, args.quality)
                    elapsed = time.monotonic() - t0
                    if elapsed < frame_interval:
                        time.sleep(frame_interval - elapsed)
                else:
                    time.sleep(0.05)
        except (serial.SerialException, OSError):
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Stream PC screen to MQ CYD USB Display")
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument("--baud", type=int, default=921600, help="Baudrate (default: 921600)")
    parser.add_argument("--width", type=int, default=320, help="Target width (default: 320)")
    parser.add_argument("--height", type=int, default=240, help="Target height (default: 240)")
    parser.add_argument("--fps", type=float, default=12.0, help="Max FPS (default: 12)")
    parser.add_argument("--quality", type=int, default=65, help="JPEG quality 20-95 (default: 65)")
    parser.add_argument("--output", default=None, help="Wayland output name for grim -o (e.g. HDMI-A-1)")
    parser.add_argument("--region", default=None, help="Capture region 'X,Y WxH' for grim -g")
    parser.add_argument("--daemon", action="store_true", help="Wait for MQDISP_READY / MQDISP_STOP automatically")
    args = parser.parse_args()

    use_grim = bool(os.environ.get("WAYLAND_DISPLAY") and shutil.which("grim"))
    if args.daemon:
        return run_daemon(args, use_grim)

    port = args.port or find_serial_port()
    if not port:
        print("Error: No /dev/ttyUSB* or /dev/ttyACM* device found.", file=sys.stderr)
        return 1

    backend = "grim (Wayland)" if use_grim else "PIL.ImageGrab"
    print(f"MQ Display Host -> {port} @ {args.baud} baud | {args.width}x{args.height} | backend: {backend}")
    print("On CYD: open Display Mode. Press Ctrl+C to stop.")

    ser = open_serial(port, args.baud)
    frame_interval = 1.0 / max(1.0, args.fps)
    frames = 0
    t0 = time.monotonic()

    try:
        while True:
            loop_start = time.monotonic()
            while ser.in_waiting:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if line:
                    print(f"[CYD] {line}")

            img = capture_frame(args.output, args.region, use_grim)
            size = send_jpeg_frame(ser, img, args.width, args.height, args.quality)
            frames += 1

            now = time.monotonic()
            if now - t0 >= 2.0:
                fps = frames / (now - t0)
                print(f"Streaming: {fps:.1f} FPS | last frame: {size} B")
                frames = 0
                t0 = now

            elapsed = time.monotonic() - loop_start
            if elapsed < frame_interval:
                time.sleep(frame_interval - elapsed)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
