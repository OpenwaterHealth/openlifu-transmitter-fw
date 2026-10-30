"""Hardware-in-the-loop fixtures: a transmitter on USB and the presets its image holds.

Run from the repo root with the openlifu SDK installed, naming the presets the
image was built with -- one preset file, or a whole preset context:

    pytest tests/hil --preset path/to/<preset>.json
    pytest tests/hil --context path/to/preset_settings/<context>

Skipped unless one is given and a transmitter answers on USB.

A presets-only (FDA_MODE) image reports its preset set as a version suffix,
+fda or +test; any other build has no suffix and is a DEV image.

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

# The version suffixes of a presets-only image: the preset context it links.
PRESETS_ONLY = ("fda", "test")


def pytest_addoption(parser):
    parser.addoption(
        "--preset",
        default=None,
        help="The preset .json the flashed image's preset 0 was generated from.",
    )
    parser.addoption(
        "--context",
        default=None,
        help="A preset context folder (presets.json, then <id>/<id>_settings.json per "
        "preset), as in openlifu-diathermy-application's preset_settings/<context>: "
        "the set a presets-only image links, in index order.",
    )


def _compile(path: Path, preset=None) -> dict:
    raw = path.read_bytes()
    # A released settings .json carries no "id": it comes from the file name,
    # as generate_presets.py named the preset built into the image.
    return compile_preset(
        json.loads(raw), preset_id=preset or preset_id(str(path)), settings_bytes=raw
    )


def _preset_set(config) -> list:
    """The presets named on the command line, in index order; [] if none."""
    preset, context = config.getoption("--preset"), config.getoption("--context")
    if preset and context:
        raise pytest.UsageError("give --preset or --context, not both")
    if preset:
        return [_compile(Path(preset))]
    if context:
        folder = Path(context)
        ids = json.loads((folder / "presets.json").read_text(encoding="utf-8"))["presets"]
        return [_compile(folder / i / f"{i}_settings.json", i) for i in ids]
    return []


def pytest_generate_tests(metafunc):
    # One case per index of the set; one case (that skips) without a set.
    if "preset_index" in metafunc.fixturenames:
        n = len(_preset_set(metafunc.config)) or 1
        metafunc.parametrize("preset_index", range(n))


@pytest.fixture(scope="session")
def preset_set(request) -> list:
    presets = _preset_set(request.config)
    if not presets:
        pytest.skip("no --preset or --context given")
    return presets


@pytest.fixture(scope="session")
def machine_config(preset_set):
    """Preset 0: the one preset, or the first of the set."""
    return preset_set[PRESET_INDEX]


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
    """The preset set of a presets-only image ("fda" or "test"), else "dev"."""
    version = txdevice.get_version()
    _, plus, suffix = version.partition("+")
    if not plus:
        return "dev"
    if suffix in PRESETS_ONLY:
        return suffix
    pytest.skip(f"image reports an unknown build variant ({version})")


@pytest.fixture(scope="session")
def preset_in_slot(txdevice, machine_config, variant):
    """Preset 0 on the device is this preset: linked in (presets-only), or stored (DEV)."""
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
    """The device as a presets-only image behaves: locked (DEV) or always locked."""
    send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOCK, reserved=1)
    assert lock_state(txdevice) == 1
