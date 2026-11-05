"""Hardware-in-the-loop fixtures: a transmitter on USB and the preset its image holds.

Run from the repo root with the openlifu SDK installed:

    pytest tests/hil --preset path/to/<preset>.json

Skipped unless --preset is given and a transmitter answers on USB.

A presets-only (FDA_MODE) image reports its preset set as a version suffix,
+fda or +test, and holds preset 0 built in. A standard image has no suffix;
on it the tests store the preset in slot 0, which needs a bare-metal image.
"""

import json
import struct
from pathlib import Path

import pytest
from openlifu_sdk.io.LIFUConfig import OW_CONTROLLER, OW_ERROR
from openlifu_sdk.io.LIFUTXDevice import LIFUTxDevice
from openlifu_sdk.io.LIFUTXPresets import (
    compile_preset,
    delete_preset,
    preset_id,
    store_preset,
    verify_preset,
)

# Not in the SDK yet (Core/Inc/common.h).
OW_PRESET_LOADED = 0x57
OW_CTRL_GET_THERMAL_LIMITS = 0x60
OW_CTRL_SET_THERMAL_LIMITS = 0x61
OW_CTRL_CLEAR_THERMAL_LIMITS = 0x62
# Sent raw, so these tests need no SDK with the pause API (SDK #89).
OW_CTRL_PAUSE_SWTRIG = 0x2B
OW_CTRL_RESUME_SWTRIG = 0x2E
# Where the armed limits came from (Core/Inc/thermal_limits.h).
LIMITS_SOURCES = {0: "none", 1: "preset", 2: "host"}

PRESET_INDEX = 0


def pytest_addoption(parser):
    parser.addoption(
        "--preset",
        default=None,
        help="The preset .json the flashed image's preset 0 was generated from.",
    )
    parser.addoption(
        "--shutoff-wait",
        type=float,
        default=300.0,
        help="Seconds test_thermal_limits waits for the TX to warm past a shutoff_C "
        "set just above the reading (warming the transducer by hand speeds it).",
    )


@pytest.fixture(scope="session")
def machine_config(request):
    path = request.config.getoption("--preset")
    if path is None:
        pytest.skip("no --preset given")
    raw = Path(path).read_bytes()
    # A released settings .json carries no "id": it comes from the file name,
    # as generate_presets.py named the preset built into the image.
    return compile_preset(json.loads(raw), preset_id=preset_id(path), settings_bytes=raw)


@pytest.fixture(scope="session")
def txdevice():
    # The research class, so the test can put commands on the wire that the
    # FDA TxDevice would refuse to send: the point is that the firmware refuses.
    tx = LIFUTxDevice()
    if not tx.connect():
        pytest.skip("no transmitter on USB")
    yield tx
    tx.close()


def send_raw(txdevice, packet_type, command, addr=0, reserved=0, data=None):
    """Put a packet on the wire as is, past the SDK's own command checks."""
    return txdevice._uart.send_packet(
        packet_type=packet_type, command=command, addr=addr, reserved=reserved, data=data
    )


def loaded(txdevice) -> dict:
    """OW_PRESET_LOADED, decoded."""
    r = send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOADED)
    assert r is not None and r.packet_type != OW_ERROR and r.data_len == 16
    d = bytes(r.data)
    train_count, settings_crc, regs_crc = struct.unpack("<III", d[3:15])
    return {
        "loaded": d[0] == 1,
        "index": d[1],
        "duration_index": d[2],
        "train_count": train_count,
        "settings_crc": settings_crc,
        "regs_crc": regs_crc,
        "missing": d[15],
    }


def decode_thermal_limits(r) -> dict:
    """A GET / SET / CLEAR_THERMAL_LIMITS reply, decoded: every one carries what is armed."""
    assert r is not None and r.packet_type != OW_ERROR and r.data_len == 8
    d = bytes(r.data)
    start, shutoff, timeout_s = struct.unpack("<hhH", d[2:8])
    return {
        "source": LIMITS_SOURCES[d[0]],
        "preset_index": None if d[1] == 0xFF else d[1],
        "x10": (start, shutoff),  # as LIFUTXPresets.thermal_limits() bakes them
        "start_c": start / 10,
        "shutoff_c": shutoff / 10,
        "treatment_timeout_s": timeout_s,  # 0: none
    }


def armed_limits(txdevice) -> dict:
    """OW_CTRL_GET_THERMAL_LIMITS, decoded."""
    return decode_thermal_limits(send_raw(txdevice, OW_CONTROLLER, OW_CTRL_GET_THERMAL_LIMITS))


def set_thermal_limits(txdevice, start_c: float, shutoff_c: float, timeout_s: int = 0, addr: int = 0):
    """OW_CTRL_SET_THERMAL_LIMITS, the raw reply. *timeout_s* 0 arms no treatment timeout."""
    data = bytearray(struct.pack("<hhH", round(start_c * 10), round(shutoff_c * 10), timeout_s))
    return send_raw(txdevice, OW_CONTROLLER, OW_CTRL_SET_THERMAL_LIMITS, addr=addr, data=data)


def clear_thermal_limits(txdevice):
    """OW_CTRL_CLEAR_THERMAL_LIMITS, the raw reply."""
    return send_raw(txdevice, OW_CONTROLLER, OW_CTRL_CLEAR_THERMAL_LIMITS)


@pytest.fixture(scope="session")
def variant(txdevice) -> str:
    """The preset set of a presets-only image ("fda" or "test"), else "standard"."""
    version = txdevice.get_version()
    _, plus, suffix = version.partition("+")
    if not plus:
        return "standard"
    if suffix in ("fda", "test"):
        return suffix
    pytest.skip(f"image reports an unknown build variant ({version})")


@pytest.fixture(scope="session")
def presets_only(txdevice, machine_config, variant):
    """A presets-only image whose preset 0 is this preset."""
    if variant == "standard":
        pytest.skip("presets-only images only: a standard image refuses none of this")
    verify_preset(txdevice, PRESET_INDEX, machine_config)


@pytest.fixture(scope="session")
def standard(variant):
    """A standard image: host control and preset slots."""
    if variant != "standard":
        pytest.skip("standard images only")


@pytest.fixture(scope="session")
def preset_in_slot(txdevice, machine_config, standard):
    """A standard image whose slot 0 holds this preset, stored if need be.

    A preset this fixture stored is deleted again at the end of the session:
    pages 110-125 left written stop a signed image booting under bootloader
    1.0.1 (#56).
    """
    stored = False
    try:
        verify_preset(txdevice, PRESET_INDEX, machine_config)
    except Exception:
        if txdevice.get_preset_capacity() is None:
            pytest.skip("a standard image under the bootloader cannot store presets")
        store_preset(txdevice, PRESET_INDEX, machine_config)
        stored = True
    verify_preset(txdevice, PRESET_INDEX, machine_config)
    yield
    if stored:
        delete_preset(txdevice, PRESET_INDEX)
