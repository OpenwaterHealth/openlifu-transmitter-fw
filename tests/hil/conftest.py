"""Hardware-in-the-loop fixtures: a transmitter on USB and the preset its image holds.

Run from the repo root with the openlifu SDK installed:

    pytest tests/hil --preset path/to/<preset>.json

Skipped unless --preset is given and a transmitter answers on USB.

On a DEV image the tests store the preset in slot 0 (bare-metal images only)
and lock the device part-way through; only a reset unlocks it again.
"""

import json
import struct
from pathlib import Path

import pytest
from openlifu_sdk.io.LIFUConfig import OW_CONTROLLER, OW_ERROR
from openlifu_sdk.io.LIFUTXDevice import LIFUTxDevice
from openlifu_sdk.io.LIFUTXPresets import (
    compile_preset,
    preset_id,
    store_preset,
    verify_preset,
)

# Not in the SDK yet (Core/Inc/common.h).
OW_PRESET_LOCK = 0x56
OW_PRESET_LOADED = 0x57

PRESET_INDEX = 0


def pytest_addoption(parser):
    parser.addoption(
        "--preset",
        default=None,
        help="The preset .json the flashed image's preset 0 was generated from.",
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


def lock_state(txdevice) -> int:
    r = send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOCK)
    assert r is not None and r.packet_type != OW_ERROR and r.data_len == 1
    return r.data[0]


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


@pytest.fixture(scope="session")
def variant(txdevice) -> str:
    version = txdevice.get_version()
    for v in ("fda", "dev"):
        if version.endswith("+" + v):
            return v
    pytest.skip(f"image reports no build variant ({version})")


@pytest.fixture(scope="session")
def preset_in_slot(txdevice, machine_config, variant):
    """Preset 0 on the device is this preset: built in (FDA), or stored (DEV)."""
    if variant == "dev":
        try:
            verify_preset(txdevice, PRESET_INDEX, machine_config)
        except Exception:
            if txdevice.get_preset_capacity() is None:
                pytest.skip("DEV image under the bootloader cannot store presets")
            store_preset(txdevice, PRESET_INDEX, machine_config)
    verify_preset(txdevice, PRESET_INDEX, machine_config)


@pytest.fixture
def locked(txdevice, preset_in_slot):
    """The device as an FDA image behaves: locked (DEV) or always locked (FDA)."""
    send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOCK, reserved=1)
    assert lock_state(txdevice) == 1
