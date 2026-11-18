from pathlib import Path
import csv
import shutil
from SCons.Script import Import

# Import PlatformIO's SCons environment
Import("env")
senv = env  # avoid shadowing with the action's 'env' parameter

# Optional: keep your extra flag
senv.Append(CXXFLAGS=["-Wno-conversion-null"])

# ---- Detect MCU and map bootloader offsets ----
board_config = senv.BoardConfig()
mcu = (board_config.get("build.mcu") or senv.get("BOARD_MCU") or "").lower()

BOOT_OFFSETS = {
    "esp32":   0x1000,  # ESP32
    "esp32s3": 0x0000,  # ESP32-S3
    "esp32c5": 0x2000,  # ESP32-C5
    "esp32c6": 0x0000,  # ESP32-C6
    "esp32p4": 0x2000,  # ESP32-P4
}
boot_offset = BOOT_OFFSETS.get(mcu, 0x0000)  # safe fallback

PART_TABLE_OFFSET = 0x8000

# Paths
build_dir = Path(senv.subst("$BUILD_DIR"))
proj_dir  = Path(senv.subst("$PROJECT_DIR"))
pioenv    = senv.subst("${PIOENV}")

boot_bin = build_dir / "bootloader.bin"
part_bin = build_dir / "partitions.bin"
app_bin  = build_dir / "firmware.bin"

is_recovery = pioenv.endswith("-recovery")
device_name = pioenv[: -len("-recovery")] if is_recovery else pioenv

part_csv_name = board_config.get("build.partitions") or senv.GetProjectOption(
    "board_build.partitions", default=""
)


def read_partitions():
    """Return {subtype: (offset, size)} from the CSV used by this build."""
    part_csv = proj_dir / part_csv_name if part_csv_name else proj_dir / "partitions.csv"
    result = {}
    if not part_csv.exists():
        return result
    with open(part_csv, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            cols = [c.strip() for c in row]
            if len(cols) < 5:
                continue
            name, ptype, subtype, offset, size = cols[:5]
            try:
                result[subtype.lower()] = (int(offset, 0), int(size, 0), name)
            except ValueError:
                continue
    return result


partitions = read_partitions()


def selected_app_partition():
    """Main app targets ota_0; the recovery app targets factory."""
    if is_recovery:
        return partitions.get("factory")
    return partitions.get("ota_0") or partitions.get("factory")


def esptool_merge(output, segments):
    esptool_pkg = senv.PioPlatform().get_package_dir("tool-esptoolpy")
    python_exe = senv.get("PYTHONEXE", "python")
    chip_arg = mcu if mcu else "esp32"
    args = []
    for offset, path in segments:
        args.extend([hex(offset), f'"{path}"'])
    cmd = " ".join(
        [
            f'"{python_exe}"',
            "-m",
            "platformio",
            "pkg",
            "exec",
            "-p",
            f'"{esptool_pkg}"',
            "--",
            "esptool.py",
            "--chip",
            chip_arg,
            "merge-bin",
            "--output",
            f'"{output}"',
        ]
        + args
    )
    print("[merge_bin]", cmd)
    return senv.Execute(cmd)


def _post_build_callback(target, source, env):
    """
    Post-action callback executed after firmware.bin is built.

    Main application:
        MQ-<device>.bin          pure application image for OTA (ota_0)
    Recovery application:
        MQ-<device>-recovery.bin bootloader + partition table + factory app
    """
    missing = [p for p in [boot_bin, part_bin, app_bin] if not p.exists()]
    if missing:
        print("[post_build] Missing files, merge aborted:")
        for p in missing:
            print(f" - {p}")
        return

    part = selected_app_partition()
    if part is None:
        print("[post_build] No factory/ota_0 partition in", part_csv_name)
        env.Exit(1)
        return
    part_offset, part_size, part_name = part

    fw_size = app_bin.stat().st_size
    percent = (fw_size / part_size) * 100 if part_size else 0
    print(
        f"MQ: {part_name} 0x{fw_size:X} / 0x{part_size:X} bytes "
        f"({percent:.1f}% used, {100.0 - percent:.1f}% free)"
    )
    if fw_size > part_size:
        print(f"[post_build] Error: firmware.bin exceeds {part_name} partition size")
        env.Exit(1)
        return

    if is_recovery:
        out_bin = proj_dir / f"MQ-{device_name}-recovery.bin"
        rc = esptool_merge(out_bin, [(boot_offset, boot_bin), (PART_TABLE_OFFSET, part_bin), (part_offset, app_bin)])
        if rc != 0:
            print("[post_build] Recovery merge failed with exit code", rc)
            env.Exit(rc)
        else:
            print(f"[post_build] Recovery image -> {out_bin} ({out_bin.stat().st_size} bytes)")
        return

    # The OTA asset must be the bare application image, not a merged flash dump.
    out_bin = proj_dir / f"MQ-{device_name}.bin"
    shutil.copyfile(app_bin, out_bin)
    print(f"[post_build] OTA application -> {out_bin} ({out_bin.stat().st_size} bytes)")


def _manual_callback(target, source, env):
    _post_build_callback(target, source, env)
    return 0


# Automatically run after firmware.bin is generated
senv.AddPostAction(str(app_bin), _post_build_callback)

senv.AddCustomTarget(
    name="build-firmware",
    dependencies=[str(boot_bin), str(part_bin), str(app_bin)],
    actions=[_manual_callback],
    title="Build Firmware (merge/copy)",
    description="Prepare the OTA application or the recovery flash image",
)

# Keep your upload-nobuild helper
senv.AddCustomTarget(
    name="upload-nobuild",
    dependencies=None,
    actions=[f"platformio run -t upload -t nobuild -e {pioenv}"],
    title="Upload Nobuild",
    description="Run upload without rebuilding",
)
