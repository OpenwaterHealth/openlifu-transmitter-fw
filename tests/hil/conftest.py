"""Hardware-in-the-loop fixtures: a transmitter on USB and the presets its image holds.

Run from the repo root with the openlifu SDK installed, naming the presets the
image was built with -- one preset file, or a whole preset context:

    pytest tests/hil --preset path/to/<preset>.json
    pytest tests/hil --context path/to/preset_settings/<context>

Skipped unless one is given and a transmitter answers on USB.

Every image reads its presets from the preset slots (pages 106-125). The
clinical (FDA_MODE) image cannot store one: its slot 0 must already hold this
preset, written through the bootloader (openlifu-sdk
LIFUDFUManager.program_transmitter_preset). On a standard image the tests store
the preset in slot 0 themselves, which needs the slots: a bare-metal image, or
one under transmitter bootloader >= 1.1.0-rc.1. The version carries no
variant; the clinical image is told apart by its refusal of a preset write.
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

# OW_PRESET_WRITE: the clinical image refuses it (OW_FDA_REFUSED), a standard
# one takes it (or refuses an empty upload as malformed).
OW_PRESET_WRITE = 0x52
OW_FDA_REFUSED = 0xF8


def pytest_addoption(parser):
    parser.addoption(
        "--preset",
        default=None,
        help="The preset .json in slot 0.",
    )
    parser.addoption(
        "--context",
        default=None,
        help="A preset context folder (presets.json, then <id>/<id>_settings.json per "
        "preset), as in openlifu-diathermy-application's preset_settings/<context>: "
        "its presets in index order; the first is the one in slot 0.",
    )
    parser.addoption(
        "--shutoff-wait",
        type=float,
        default=300.0,
        help="Seconds test_thermal_limits waits for the TX to warm past a shutoff_C "
        "set just above the reading (warming the transducer by hand speeds it).",
    )


def _compile(path: Path, preset=None) -> dict:
    raw = path.read_bytes()
    # A released settings .json carries no "id": it comes from the file name,
    # as the diathermy application names its presets.
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
    """"fda" for the clinical (FDA_MODE) image, else "standard": told by its
    refusal of an (empty) preset write. Nothing is written either way."""
    r = send_raw(txdevice, OW_CONTROLLER, OW_PRESET_WRITE)
    assert r is not None, "no reply to a preset write"
    return "fda" if (r.packet_type, r.reserved) == (OW_ERROR, OW_FDA_REFUSED) else "standard"


@pytest.fixture(scope="session")
def presets_only(txdevice, machine_config, variant):
    """The clinical image, whose preset 0 is this preset."""
    if variant == "standard":
        pytest.skip("the clinical image only: a standard image refuses none of this")
    try:
        verify_preset(txdevice, PRESET_INDEX, machine_config)
    except Exception as e:
        pytest.skip(f"slot {PRESET_INDEX} does not hold this preset ({e}): write it through "
                    "the bootloader (LIFUDFUManager.program_transmitter_preset)")


@pytest.fixture(scope="session")
def standard(variant):
    """A standard image: host control and preset slots."""
    if variant != "standard":
        pytest.skip("standard images only")


@pytest.fixture(scope="session")
def preset_in_slot(txdevice, machine_config, standard):
    """A standard image whose slot 0 holds this preset, stored if need be.

    A preset this fixture stored is deleted again at the end of the session.
    """
    stored = False
    try:
        verify_preset(txdevice, PRESET_INDEX, machine_config)
    except Exception:
        capacity = txdevice.get_preset_capacity()
        if not capacity or capacity["slots"] == 0:
            pytest.skip("no preset slots: a standard image under a bootloader before 1.1.0-rc.1")
        store_preset(txdevice, PRESET_INDEX, machine_config)
        stored = True
    verify_preset(txdevice, PRESET_INDEX, machine_config)
    yield
    if stored:
        delete_preset(txdevice, PRESET_INDEX)
