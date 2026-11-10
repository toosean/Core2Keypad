#!/usr/bin/env python3
"""Grab the M5Stack Core2 screen over USB.

The firmware keeps its 320x240 logical frame in an M5Canvas and answers
``C2_SCREEN`` with that frame, so the PNG matches what the renderer drew
regardless of panel rotation:

    python tools/c2_screenshot.py
    python tools/c2_screenshot.py --open
    python tools/c2_screenshot.py --port COM6 --output hi.png

Saved files default to ``outputs/screenshots/c2-<time>.png`` and the same
frame is copied to ``latest.png`` for quick previewing.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / "outputs" / "screenshots"
DEFAULT_BAUD = 921600          # must match Serial.begin() in src/main.cpp
HEADER_TIMEOUT_S = 8
FRAME_TIMEOUT_S = 30

# M5Stack Core2 ships a WCH CH9102 USB-UART bridge (newer batches; older ones
# use a CP210x).  Match the bridge chips that dev boards actually use.
BRIDGE_IDS = {
    (0x1A86, 0x55D4),   # CH9102
    (0x1A86, 0x7523),   # CH340
    (0x10C4, 0xEA60),   # CP2102
    (0x0403, 0x6001),   # FT232
}
BRIDGE_HINT = ("CH9102", "CH340", "CP210", "USB-Enhanced-SERIAL", "USB 串行设备")


def describe(port) -> str:
    return f"{port.device} ({port.description or 'unknown'})"


def core2_candidates():
    """Return serial ports that look like an M5Stack dev board."""
    found = []
    for port in list_ports.comports():
        hwid = (port.hwid or "").upper()
        text = f"{port.description or ''} {port.product or ''} {port.manufacturer or ''}"
        if port.vid is not None and (port.vid, port.pid) in BRIDGE_IDS:
            found.append(port)
        elif any(k.upper() in hwid for k in ("1A86:55D4", "1A86:7523", "10C4:EA60", "0403:6001")):
            found.append(port)
        elif any(k in text for k in BRIDGE_HINT):
            found.append(port)
    return found


def pick_port(explicit: str | None, wait_s: float) -> str:
    if explicit:
        return explicit
    deadline = time.monotonic() + max(wait_s, 0)
    while True:
        candidates = core2_candidates()
        if len(candidates) == 1:
            return candidates[0].device
        if len(candidates) > 1:
            listing = ", ".join(describe(p) for p in candidates)
            raise SystemExit(f"找到多个疑似串口，请用 --port 指定：{listing}")
        if time.monotonic() >= deadline:
            others = ", ".join(describe(p) for p in list_ports.comports()) or "无"
            raise SystemExit(
                "没有检测到开发板的 USB 串口（CH9102/CH340/CP210x/FTDI）。"
                "请确认 Core2 已开机并用数据线连到这台电脑。"
                f"当前串口：{others}"
            )
        time.sleep(.5)


def _request_header(link, timeout: float):
    """Send C2_SCREEN and wait for the header line; None on timeout."""
    link.reset_input_buffer()
    link.write(b"C2_SCREEN\n")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = link.readline()
        if line.startswith(b"C2_SCREEN "):
            return line.split()
        if line.startswith(b"C2_ERROR"):
            raise RuntimeError(line.strip().decode("utf-8", "replace"))
    return None


def _pulse_reset(link):
    """EN pulse (RTS) with GPIO0 left high (DTR low) -> normal boot."""
    link.dtr = False
    link.rts = True
    time.sleep(.12)
    link.rts = False


def capture(port_name: str, baud: int, reset: bool = True) -> Image.Image:
    """Return a PIL image containing the Core2 logical frame."""
    link = serial.Serial(baudrate=baud, timeout=.2, write_timeout=5)
    # Assert DTR/RTS low before opening so the host does not reset the board.
    link.dtr = False
    link.rts = False
    link.port = port_name
    try:
        link.open()

        # The board may still sit in the ROM downloader right after a flash, so
        # the first attempt is short: if it stays silent, pulse EN and retry.
        header = _request_header(link, 2.5)
        if header is None and reset:
            print(f"{port_name} 无响应，复位后重试…", file=sys.stderr)
            _pulse_reset(link)
            time.sleep(2.0)
            header = _request_header(link, HEADER_TIMEOUT_S)
        if header is None:
            raise TimeoutError(
                "没有收到 C2_SCREEN 响应。请确认设备跑的是本工程固件、"
                f"波特率是 {baud}，且串口没有被 pio device monitor 等程序占用。"
            )

        width, height, length = map(int, header[1:4])
        if width <= 0 or height <= 0 or length != width * height * 3:
            raise RuntimeError(f"意外的画面参数：{width}x{height}, {length} 字节")

        data = bytearray()
        deadline = time.monotonic() + FRAME_TIMEOUT_S
        while len(data) < length and time.monotonic() < deadline:
            chunk = link.read(min(262144, length - len(data)))
            if chunk:
                data.extend(chunk)
        if len(data) != length:
            raise TimeoutError(f"画面传输不完整：{len(data)}/{length} 字节")
        return Image.frombytes("RGB", (width, height), bytes(data))
    finally:
        link.close()


def main():
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="Take an M5Stack Core2 screenshot over USB.")
    parser.add_argument("--port", help="serial port such as COM6; auto-detected by default")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"default {DEFAULT_BAUD}")
    parser.add_argument("--output", type=Path, help="target PNG; defaults to outputs/screenshots/")
    parser.add_argument("--wait", type=float, default=0,
                        help="seconds to wait for the board to appear (default: 0)")
    parser.add_argument("--no-reset", action="store_true",
                        help="never pulse the reset line (default: reset only if the board is silent)")
    parser.add_argument("--open", action="store_true", help="open the PNG when done")
    args = parser.parse_args()

    port = pick_port(args.port, args.wait)
    target = args.output or DEFAULT_DIR / f"c2-{datetime.now():%Y%m%d-%H%M%S}.png"
    try:
        image = capture(port, args.baud, reset=not args.no_reset)
    except serial.SerialException as exc:
        raise SystemExit(f"无法使用串口 {port}：{exc}") from None

    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target)
    print(f"Core2 {image.width}x{image.height} -> {target.resolve()}")

    if args.output is None:
        latest = DEFAULT_DIR / "latest.png"
        shutil.copyfile(target, latest)
        print(f"latest: {latest.resolve()}")

    if args.open:
        if sys.platform == "win32":
            os.startfile(target)  # noqa: S606 - local preview convenience
        else:
            print("--open is only implemented on Windows", file=sys.stderr)


if __name__ == "__main__":
    main()
