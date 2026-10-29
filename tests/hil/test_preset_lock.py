"""The DEV build's runtime preset lock, and OW_PRESET_LOADED on any build.

Runs before test_preset_refusals.py (file order): the unlocked checks need a
DEV image that has not been locked since its last reset. A bare-metal DEV image
is needed to store the preset. Keep the console HV off.
"""

import json

import pytest
from conftest import (
    OW_PRESET_LOCK,
    PRESET_INDEX,
    loaded,
    lock_state,
    send_raw,
)
from openlifu_sdk.io.exceptions import LIFUDeviceError
from openlifu_sdk.io.LIFUConfig import (
    OW_CMD,
    OW_CMD_USR_CFG,
    OW_CONTROLLER,
    OW_CTRL_SET_DELAY_PROFILE,
    OW_CTRL_SET_PATTERN_PROFILE,
    OW_CTRL_SET_PROFILE_CYCLE,
    OW_CTRL_SET_SWTRIG,
    OW_ERROR,
    OW_FDA_REFUSED,
    OW_I2C_PASSTHRU,
    OW_INVALID_PACKET,
    OW_NO_PRESET,
    OW_PRESET_COMMIT,
    OW_PRESET_DELETE,
    OW_PRESET_WRITE,
    OW_TX7332,
    OW_TX7332_RREG,
    OW_TX7332_WREG,
)
from openlifu_sdk.io.LIFUTXPresets import load_preset

LAST_SLOT = 15


def _trigger_json(txdevice) -> bytearray:
    return bytearray(json.dumps(txdevice.get_trigger_json()).encode("ascii"))


# -- unlocked (DEV only) ------------------------------------------------------


def test_dev_starts_unlocked(txdevice, variant):
    if variant != "dev":
        pytest.skip("FDA images are always locked")
    assert lock_state(txdevice) == 0, "reset the DEV image before this run"


def test_unlocked_raw_write_goes_through_and_unloads(
    txdevice, machine_config, variant, preset_in_slot
):
    if variant != "dev" or lock_state(txdevice):
        pytest.skip("needs an unlocked DEV image")
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    assert loaded(txdevice)["loaded"]

    r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_SET_SWTRIG, data=_trigger_json(txdevice))

    assert r is not None and r.packet_type != OW_ERROR, "unlocked, the write goes through"
    state = loaded(txdevice)
    assert not state["loaded"], "a raw write leaves nothing loaded"
    assert state["missing"] & 1


def test_unlocked_start_with_nothing_loaded_runs_the_host_config(txdevice, variant):
    if variant != "dev" or lock_state(txdevice):
        pytest.skip("needs an unlocked DEV image")
    assert not loaded(txdevice)["loaded"]
    # As on a research build: the host's own trigger config, no preset gate.
    txdevice.start_trigger()
    txdevice.stop_trigger()


def test_a_raw_write_on_one_module_reads_as_nothing_loaded(
    txdevice, machine_config, variant, preset_in_slot
):
    if variant != "dev" or lock_state(txdevice):
        pytest.skip("needs an unlocked DEV image")
    if txdevice.get_module_count() < 2:
        pytest.skip("needs two modules")
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    assert loaded(txdevice)["loaded"]

    # Read a register on module 1's first chip and write the same value back:
    # module 1 runs the write and unloads, the master only forwards it.
    chip = 2
    r = send_raw(txdevice, OW_TX7332, OW_TX7332_RREG, addr=chip, data=bytearray(b"\x00\x00"))
    assert r is not None and r.packet_type != OW_ERROR
    value = bytes(r.data[:4])
    w = send_raw(
        txdevice, OW_TX7332, OW_TX7332_WREG, addr=chip, data=bytearray(b"\x00\x00" + value)
    )
    assert w is not None and w.packet_type != OW_ERROR

    state = loaded(txdevice)
    assert not state["loaded"], "one module without the load reads as nothing loaded"
    assert state["missing"] == 0b10, "module 1 unloaded itself; the master still holds the load"


# -- the lock -----------------------------------------------------------------


def test_the_lock_turns_on_and_stays_on(txdevice, preset_in_slot):
    send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOCK, reserved=1)
    assert lock_state(txdevice) == 1
    # Nothing but a reset turns it off.
    r = send_raw(txdevice, OW_CONTROLLER, OW_PRESET_LOCK, reserved=2)
    assert r.packet_type == OW_ERROR and r.reserved == OW_INVALID_PACKET
    assert lock_state(txdevice) == 1


@pytest.mark.parametrize(
    "packet_type, command, reserved, data, code",
    [
        (OW_CONTROLLER, OW_CTRL_SET_SWTRIG, 0, b"{}", OW_INVALID_PACKET),
        (OW_CONTROLLER, OW_CTRL_SET_DELAY_PROFILE, 0, b"\x00", OW_INVALID_PACKET),
        (OW_CONTROLLER, OW_CTRL_SET_PATTERN_PROFILE, 0, b"\x00", OW_INVALID_PACKET),
        (OW_CONTROLLER, OW_CTRL_SET_PROFILE_CYCLE, 0, b"\x00", OW_INVALID_PACKET),
        (OW_TX7332, OW_TX7332_WREG, 0, b"\x00\x00\x00\x00\x00\x00", OW_INVALID_PACKET),
        # A malformed body: refused by the lock before anything is parsed.
        (OW_CMD, OW_CMD_USR_CFG, 1, b"{", OW_FDA_REFUSED),
        # A read of zero bytes from address 0x00: harmless even if it ran.
        (OW_I2C_PASSTHRU, 0x01, 0, None, OW_FDA_REFUSED),
        (OW_CONTROLLER, OW_PRESET_WRITE, LAST_SLOT, b"\x00\x00\x00", OW_FDA_REFUSED),
        (OW_CONTROLLER, OW_PRESET_COMMIT, LAST_SLOT, b"\x00" * 8, OW_FDA_REFUSED),
        (OW_CONTROLLER, OW_PRESET_DELETE, LAST_SLOT, None, OW_FDA_REFUSED),
    ],
)
def test_locked_refuses_what_fda_refuses(
    txdevice, variant, locked, packet_type, command, reserved, data, code
):
    if variant == "fda" and command in (OW_PRESET_WRITE, OW_PRESET_COMMIT, OW_PRESET_DELETE):
        pytest.skip("the FDA image has no stored presets")
    r = send_raw(
        txdevice, packet_type, command, reserved=reserved, data=bytearray(data) if data else None
    )
    assert r is not None
    assert (r.packet_type, r.reserved) == (OW_ERROR, code)


def test_locked_reads_stay_open(txdevice, locked):
    assert txdevice.get_trigger_json()
    txdevice.read_config()
    r = send_raw(txdevice, OW_TX7332, OW_TX7332_RREG, addr=0, data=bytearray(b"\x00\x00"))
    assert r is not None and r.packet_type != OW_ERROR


def test_locked_start_needs_a_preset(txdevice, locked):
    for module in range(txdevice.get_module_count()):
        txdevice.unload_preset(module)
    with pytest.raises(LIFUDeviceError) as e:
        txdevice.start_trigger()
    assert e.value.device_error_code == OW_NO_PRESET
