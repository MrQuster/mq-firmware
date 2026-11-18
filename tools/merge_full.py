#!/usr/bin/env python3
"""Build the single-file USB recovery image for the 4 MB CYD.

Layout written by this script (offsets come from partitions.csv):

    0x1000   bootloader.bin
    0x8000   partitions.bin
    0x10000  factory  recovery application
    0x100000 ota_0    main MQ application

otadata is intentionally omitted.  esptool fills the gap with 0xFF, the ESP32
bootloader then starts the factory recovery image, and the recovery image
validates ota_0 and hands control to it.  nvs (0x3C0000) and LittleFS
(0x3D0000) sit above the last segment and are therefore never touched by a
full USB flash.

Usage:
    python tools/merge_full.py
    python tools/merge_full.py --device CYD-2432S028 --output dist/...
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_ENV = "CYD-2432S028"
RECOVERY_ENV = "CYD-2432S028-recovery"
DEVICE = "CYD-2432S028"
BOOTLOADER_OFFSET = 0x1000
PARTITION_TABLE_OFFSET = 0x8000


def parse_partitions(path: Path) -> dict[str, tuple[int, int, str]]:
    result: dict[str, tuple[int, int, str]] = {}
    with path.open(newline="") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            cols = [c.strip() for c in row]
            if len(cols) < 5:
                continue
            name, _ptype, subtype, offset, size = cols[:5]
            try:
                result[subtype.lower()] = (int(offset, 0), int(size, 0), name)
            except ValueError:
                continue
    return result


def esptool_command() -> list[str]:
    """Return a command prefix that can run esptool.

    The system Python does not have esptool's runtime dependencies, so prefer
    PlatformIO's virtualenv and fall back to the current interpreter.
    """
    penv = Path.home() / ".platformio" / "penv" / "bin" / "python"
    python = penv if penv.exists() else Path(sys.executable)
    esptool = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
    if esptool.exists():
        return [str(python), str(esptool)]
    return [str(python), "-m", "esptool"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default=DEVICE)
    parser.add_argument("--output", default=None)
    parser.add_argument("--partitions", default=str(ROOT / "partitions.csv"))
    args = parser.parse_args()

    partitions = parse_partitions(Path(args.partitions))
    factory = partitions.get("factory")
    ota0 = partitions.get("ota_0")
    if factory is None or ota0 is None:
        raise SystemExit("partitions.csv must define factory and ota_0 app partitions")

    main_build = ROOT / ".pio" / "build" / MAIN_ENV
    recovery_build = ROOT / ".pio" / "build" / RECOVERY_ENV

    inputs = {
        "bootloader": main_build / "bootloader.bin",
        "partition table": main_build / "partitions.bin",
        "recovery app": recovery_build / "firmware.bin",
        "main app": main_build / "firmware.bin",
    }
    for label, path in inputs.items():
        if not path.exists():
            raise SystemExit(f"missing {label}: {path}\nRun both `pio run -e {MAIN_ENV}` and `pio run -e {RECOVERY_ENV}` first")

    for label, value in (("recovery", factory), ("main", ota0)):
        offset, size, _name = value
        app_size = inputs["recovery app" if label == "recovery" else "main app"].stat().st_size
        if app_size > size:
            raise SystemExit(f"{label} app {app_size} exceeds partition size {size}")

    output = Path(args.output) if args.output else ROOT / f"MQ-{args.device}-full.bin"
    output.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        *esptool_command(),
        "--chip",
        "esp32",
        "merge-bin",
        "--flash-mode",
        "dio",
        "--flash-freq",
        "40m",
        "--flash-size",
        "4MB",
        "--output",
        str(output),
        hex(BOOTLOADER_OFFSET),
        str(inputs["bootloader"]),
        hex(PARTITION_TABLE_OFFSET),
        str(inputs["partition table"]),
        hex(factory[0]),
        str(inputs["recovery app"]),
        hex(ota0[0]),
        str(inputs["main app"]),
    ]
    print("[full] " + " ".join(cmd))
    subprocess.run(cmd, check=True)

    size = output.stat().st_size
    end = ota0[0] + inputs["main app"].stat().st_size
    print(f"[full] {output} : {size} bytes, last written offset 0x{end:X}")
    print(f"[full] nvs     0x{partitions['nvs'][0]:X}-0x{partitions['nvs'][0] + partitions['nvs'][1]:X} preserved")
    print(
        f"[full] spiffs  0x{partitions['spiffs'][0]:X}-0x{partitions['spiffs'][0] + partitions['spiffs'][1]:X} preserved"
    )
    if end > partitions["nvs"][0]:
        raise SystemExit("main app overlaps the nvs partition")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
