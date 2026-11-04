"""A standard image's preset slots, and the host's own control beside them.

Needs a bare-metal standard image (no version suffix) to store the preset in
slot 0 (conftest.py); skipped on +fda / +test. There is no runtime lock: a
raw register, profile or trigger write goes through and leaves nothing loaded,
unless a loaded preset is running. Keep the console HV off.
"""

import json

import pytest
from conftest import PRESET_INDEX, loaded, send_raw
from openlifu_sdk.io.LIFUConfig import (
    OW_CONTROLLER,
    OW_CTRL_SET_SWTRIG,
    OW_ERROR,
    OW_PRESET_BUSY,
    OW_TX7332,
    OW_TX7332_RREG,
    OW_TX7332_WREG,
)
from openlifu_sdk.io.LIFUTXPresets import (
    delete_preset,
    load_preset,
    store_preset,
    verify_preset,
)

LAST_SLOT = 15
# The retired runtime lock's command (Core/Inc/common.h).
RETIRED_PRESET_LOCK = 0x56


def _trigger_json(txdevice) -> bytearray:
    return bytearray(json.dumps(txdevice.get_trigger_json()).encode("ascii"))


def test_store_load_delete(txdevice, machine_config, preset_in_slot):
    capacity = txdevice.get_preset_capacity()
    assert capacity["slots"] == 16

    store_preset(txdevice, LAST_SLOT, machine_config)
    verify_preset(txdevice, LAST_SLOT, machine_config)
    load_preset(txdevice, LAST_SLOT, machine_config, duration_index=0)
    state = loaded(txdevice)
    assert state["loaded"] and state["index"] == LAST_SLOT

    delete_preset(txdevice, LAST_SLOT)
    assert txdevice.get_preset_slot(LAST_SLOT)["state"] == "empty"
    assert not loaded(txdevice)["loaded"], "deleting the loaded preset's slot unloads it"


def test_raw_write_goes_through_and_unloads(txdevice, machine_config, preset_in_slot):
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    assert loaded(txdevice)["loaded"]

    r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_SET_SWTRIG, data=_trigger_json(txdevice))

    assert r is not None and r.packet_type != OW_ERROR, "the host's write goes through"
    state = loaded(txdevice)
    assert not state["loaded"], "a raw write leaves nothing loaded"
    assert state["missing"] & 1


def test_start_with_nothing_loaded_runs_the_host_config(txdevice, standard):
    for module in range(txdevice.get_module_count()):
        txdevice.unload_preset(module)
    # The host's own trigger config, with no preset gate.
    txdevice.start_trigger()
    txdevice.stop_trigger()


def test_raw_write_while_a_preset_runs_is_busy(txdevice, machine_config, preset_in_slot):
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    txdevice.start_trigger()
    try:
        r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_SET_SWTRIG, data=_trigger_json(txdevice))
        assert r is not None
        assert (r.packet_type, r.reserved) == (OW_ERROR, OW_PRESET_BUSY)
        assert loaded(txdevice)["loaded"], "the refused write changed nothing"
    finally:
        txdevice.stop_trigger()


def test_a_raw_write_on_one_module_reads_as_nothing_loaded(
    txdevice, machine_config, preset_in_slot
):
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


def test_there_is_no_lock_command(txdevice, standard):
    r = send_raw(txdevice, OW_CONTROLLER, RETIRED_PRESET_LOCK, reserved=1)
    assert r is not None and r.packet_type == OW_ERROR, "0x56 is an unknown command"
    # And nothing was locked: the host's own writes still go through.
    w = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_SET_SWTRIG, data=_trigger_json(txdevice))
    assert w is not None and w.packet_type != OW_ERROR
