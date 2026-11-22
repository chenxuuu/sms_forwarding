#!/usr/bin/env python3
"""串口日志抓取工具（不触发复位）。

用法:
    python tools/serial_capture.py COM3 60            # 抓 60 秒
    python tools/serial_capture.py COM3 60 --reset     # 打开时先复位设备

说明：ESP32-C3 的 USB-Serial/JTAG 在串口被打开时可能因 DTR/RTS 电平变化而复位，
本脚本打开端口后立即把 DTR/RTS 置低，因此可以观察到设备“当前”的状态（包括崩溃现场）。
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM3")
    ap.add_argument("seconds", nargs="?", type=float, default=30.0)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true", help="打开端口时复位设备")
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port
    s.baudrate = args.baud
    s.timeout = 0.5
    s.dtr = args.reset
    s.rts = args.reset
    s.open()
    if not args.reset:
        try:
            s.dtr = False
            s.rts = False
        except Exception:
            pass

    end = time.time() + args.seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
