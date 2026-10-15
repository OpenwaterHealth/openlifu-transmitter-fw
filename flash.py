"""Build (optionally) and flash the transmitter firmware over ST-LINK.

    python flash.py                                    # flash the existing Debug-BareMetal image
    python flash.py --build                            # bump the build number, rebuild, then flash
    python flash.py --preset Release-Preset-BareMetal --build
    python flash.py --build --no-increment             # rebuild without bumping the counter
    python flash.py --list-probes                      # enumerate probes, flash nothing

Any preset in CMakePresets.json can be flashed. Each --build bumps a counter in
build/.localbuild and bakes it into FW_VERSION as "+bN", so the version the
device reports back confirms the new image actually took.

The probe is chosen by what it is wired to, not by position: with several
ST-LINKs attached, the one whose target is this board's chip is used. Pass
--probe-sn to name one.

The chip is programmed under reset, so the firmware and its watchdog are not
running while the download is. Only the pages the image covers are erased:
stored presets and the config page are left as they were.

A preset that is not bare metal links the image for the secure bootloader's
slot, which only boots it once it is signed, and the keys are not on a bench
PC. The script asks before flashing one unsigned.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BOARD = "transmitter"
DEVICE_ID = "0x435"                  # STM32L43x/L44x
DEFAULT_PRESET = "Debug-BareMetal"
DEFAULT_FREQ_KHZ = None              # None leaves it to the probe
AFTER_FLASH = ("If the transmitter does not show up on USB, cycle its 12 V: after an ST-LINK "
               "flash it can start in the chip's own loader instead of the firmware.")

REPO = Path(__file__).resolve().parent
CUBE_PLUGINS = r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins"
VERSION_LINE = re.compile(rb'(?m)^(#define FW_VERSION[ \t]+")([^"]*?)(\+b\d+)?(")')


def find_programmer() -> str:
    found = shutil.which("STM32_Programmer_CLI")
    if found:
        return found
    candidates = [Path(os.environ.get(var, "")) / r"STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
                  for var in ("ProgramFiles", "ProgramFiles(x86)")]
    # Fall back to the copy bundled with STM32CubeIDE.
    candidates += [Path(p) for p in glob.glob(CUBE_PLUGINS + r"\*cubeprogrammer*\tools\bin\STM32_Programmer_CLI.exe")]
    for exe in candidates:
        if exe.is_file():
            return str(exe)
    sys.exit("STM32_Programmer_CLI.exe not found. Install STM32CubeProgrammer.")


def add_toolchain_to_path() -> None:
    # arm-none-eabi-gcc and ninja ship inside CubeIDE under versioned folder names.
    for pattern in ("*externaltools.gnu-tools-for-stm32*", "*externaltools.ninja*"):
        for tools in glob.glob(CUBE_PLUGINS + "\\" + pattern + r"\tools\bin"):
            if tools not in os.environ["PATH"]:
                os.environ["PATH"] = tools + os.pathsep + os.environ["PATH"]
    for tool in ("arm-none-eabi-gcc", "ninja", "cmake"):
        if not shutil.which(tool):
            sys.exit("%s not found on PATH or in an STM32CubeIDE install." % tool)


def cmake_presets() -> dict:
    doc = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
    names = {b["name"] for b in doc.get("buildPresets", [])}
    return {c["name"]: c.get("cacheVariables", {}) for c in doc["configurePresets"] if c["name"] in names}


def step_build_number(increment: bool) -> int:
    # One counter for every preset. It lives under build/, so it is gitignored
    # and deleting build/ starts it again at 1.
    counter = REPO / "build" / ".localbuild"
    try:
        n = int(counter.read_bytes().decode("utf-8-sig").strip())
    except (OSError, ValueError):
        n = 0
    n += increment
    counter.parent.mkdir(exist_ok=True)
    counter.write_text(str(n), encoding="utf-8")
    return n


def version_header(preset: str) -> Path:
    return REPO / "build" / preset / "generated" / "version.h"


def stamp_build_number(preset: str, number: int) -> None:
    # CMake only regenerates version.h at configure time, so the build number
    # is written straight into the generated header, replacing any earlier one.
    header = version_header(preset)
    if not header.is_file():
        sys.exit("version.h not generated at %s" % header)
    text = header.read_bytes()
    patched, hits = VERSION_LINE.subn(lambda m: m.group(1) + m.group(2) + b"+b%d" % number + m.group(4), text)
    if not hits:
        sys.exit("could not find FW_VERSION in %s" % header)
    header.write_bytes(patched)


def build(preset: str, increment: bool) -> None:
    add_toolchain_to_path()
    number = step_build_number(increment)
    print("Building   : %s (local build b%d)" % (preset, number))
    # Configure first, on its own: left to ninja, the regeneration would rewrite
    # version.h mid-build and drop the stamp. cmake reads presets from the cwd.
    subprocess.run(["cmake", "--preset", preset], cwd=REPO, check=True)
    stamp_build_number(preset, number)
    subprocess.run(["cmake", "--build", "--preset", preset], cwd=REPO, check=True)


def newest_hex(preset: str) -> Path:
    images = sorted((REPO / "build" / preset).glob("*.hex"), key=lambda p: p.stat().st_mtime)
    if not images:
        sys.exit("No .hex in build\\%s. Run with --build first." % preset)
    return images[-1]


def report_version(preset: str, image: Path) -> None:
    # Checked against the .bin, not the header, so a stale build cannot pass
    # for a fresh one.
    header = version_header(preset)
    match = VERSION_LINE.search(header.read_bytes()) if header.is_file() else None
    if not match:
        return
    version = match.group(2) + (match.group(3) or b"")
    binary = image.with_suffix(".bin")
    stale = "" if binary.is_file() and version in binary.read_bytes() else "  (NOT found in the .bin - image may be stale)"
    print("Version    : %s%s" % (version.decode(), stale))


def probe_serials(cli: str) -> list:
    out = subprocess.run([cli, "-l", "stlink"], capture_output=True, text=True).stdout
    return re.findall(r"ST-LINK SN\s*:\s*(\S+)", out)


def target_id(cli: str, serial: str) -> str:
    """Device ID of the chip a probe is wired to, or "" if it cannot be read."""
    out = subprocess.run([cli, "-c", "port=SWD", "sn=" + serial, "mode=HOTPLUG"],
                         capture_output=True, text=True).stdout
    match = re.search(r"Device ID\s*:\s*(0x[0-9A-Fa-f]+)", out)
    return match.group(1).lower() if match else ""


def pick_probe(cli: str, wanted: str) -> str:
    serials = probe_serials(cli)
    if not serials:
        sys.exit("No ST-LINK probe found.")
    if wanted:
        if wanted not in serials:
            sys.exit("Probe %s is not attached (found: %s)." % (wanted, ", ".join(serials)))
        return wanted
    targets = {sn: target_id(cli, sn) for sn in serials}
    mine = [sn for sn, dev in targets.items() if dev == DEVICE_ID]
    if len(mine) == 1:
        return mine[0]
    seen = ", ".join("%s -> %s" % (sn, dev or "no target") for sn, dev in targets.items())
    if not mine:
        sys.exit("No probe is wired to a %s (device ID %s). Found: %s" % (BOARD, DEVICE_ID, seen))
    sys.exit("More than one probe reports device ID %s; pick one with --probe-sn. Found: %s" % (DEVICE_ID, seen))


def main() -> int:
    sys.stdout.reconfigure(line_buffering=True)   # keep our lines in order with the programmer's
    presets = cmake_presets()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default=DEFAULT_PRESET, choices=sorted(presets), help="CMake preset to flash (default: %(default)s)")
    ap.add_argument("--build", action="store_true", help="rebuild the preset before flashing")
    ap.add_argument("--no-increment", action="store_true", help="with --build, reuse the previous local build number")
    ap.add_argument("--probe-sn", default="", help="serial number of the probe to use")
    ap.add_argument("--list-probes", action="store_true", help="show the attached probes and their targets, then exit")
    ap.add_argument("--freq", type=int, default=DEFAULT_FREQ_KHZ, help="SWD clock in kHz (default: %(default)s)")
    ap.add_argument("--force", action="store_true", help="flash an unsigned bootloader-slot image without asking")
    args = ap.parse_args()

    cli = find_programmer()
    print("Programmer : %s" % cli)
    if args.list_probes:
        for sn in probe_serials(cli) or ["(none)"]:
            print("  %s -> %s" % (sn, target_id(cli, sn) or "no target") if sn != "(none)" else "  no probes")
        return 0

    if presets[args.preset].get("BARE_METAL") != "ON" and not args.force:
        print("\nWARNING: '%s' links the image for the secure bootloader's slot, which boots it\n"
              "         only once it is signed. Flashed unsigned, the unit will not boot until\n"
              "         a signed image is installed. For bench work use a bare-metal preset.\n" % args.preset)
        if not input("Flash it anyway? [y/N] ").strip().lower().startswith("y"):
            print("Aborted.")
            return 1

    if args.build:
        build(args.preset, not args.no_increment)
    image = newest_hex(args.preset)
    print("Image      : %s" % image)
    report_version(args.preset, image)

    serial = pick_probe(cli, args.probe_sn)
    connect = ["port=SWD", "sn=" + serial, "mode=UR", "reset=HWrst"]
    if args.freq:
        connect.append("freq=%d" % args.freq)
    print("Programming: probe %s, %s" % (serial, " ".join(connect[2:])))
    if subprocess.run([cli, "-c", *connect, "-d", str(image), "--verify", "-rst"]).returncode != 0:
        sys.exit("programming failed. Check the SWD wiring and that the %s is powered." % BOARD)
    print("Flashed OK.")
    if AFTER_FLASH:
        print(AFTER_FLASH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
