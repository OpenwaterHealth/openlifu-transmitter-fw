"""A locked device refuses host writes that would change what a loaded preset runs.

Runs on an FDA image (always locked) or a DEV image, which the `locked` fixture
locks (conftest.py). Keep the console HV off: nothing here turns HV on.
"""

import json

import pytest
from conftest import PRESET_INDEX, loaded
from openlifu_sdk.io.exceptions import LIFUDeviceError
from openlifu_sdk.io.LIFUConfig import (
    OW_CONTROLLER,
    OW_CTRL_SET_SWTRIG,
    OW_ERROR,
    OW_INVALID_PACKET,
)
from openlifu_sdk.io.LIFUTXPresets import load_preset, regs_crc, train_count_selections


def _set_swtrig(txdevice, trigger: dict):
    return txdevice.send(
        packet_type=OW_CONTROLLER,
        command=OW_CTRL_SET_SWTRIG,
        data=bytearray(json.dumps(trigger).encode("ascii")),
    )


def test_set_swtrig_is_refused_after_a_load(txdevice, machine_config, locked):
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    before = txdevice.get_trigger_json()

    other = dict(before)
    other["TriggerPulseCount"] = before["TriggerPulseCount"] * 2
    other["TriggerPulseTrainCount"] = before["TriggerPulseTrainCount"] + 1
    r = _set_swtrig(txdevice, other)

    assert r is not None, "no reply to SET_SWTRIG"
    assert r.packet_type == OW_ERROR
    assert r.reserved == OW_INVALID_PACKET
    assert txdevice.get_trigger_json() == before
    assert loaded(txdevice)["loaded"], "a refused write must leave the load alone"


def test_set_swtrig_is_refused_with_nothing_loaded(txdevice, machine_config, locked):
    for module in range(txdevice.get_module_count()):
        txdevice.unload_preset(module)
    before = txdevice.get_trigger_json()

    r = _set_swtrig(txdevice, before)

    assert r is not None and r.packet_type == OW_ERROR
    assert r.reserved == OW_INVALID_PACKET


@pytest.mark.parametrize("duration_index", range(8))
def test_each_run_length_is_selected_through_the_load(
    txdevice, machine_config, locked, duration_index
):
    counts = train_count_selections(machine_config)
    if duration_index >= len(counts):
        pytest.skip(f"the preset offers {len(counts)} run lengths")

    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=duration_index)

    assert txdevice.get_trigger_json()["TriggerPulseTrainCount"] == counts[duration_index]
    state = loaded(txdevice)
    assert state["loaded"] and state["missing"] == 0
    assert (state["index"], state["duration_index"]) == (PRESET_INDEX, duration_index)
    assert state["train_count"] == counts[duration_index]
    assert state["settings_crc"] == machine_config["settings_crc"]
    assert state["regs_crc"] == regs_crc(machine_config, 0)


def test_a_run_length_the_preset_does_not_offer_is_refused(txdevice, machine_config, locked):
    counts = train_count_selections(machine_config)
    # The right CRCs, so the duration index is the only thing wrong.
    with pytest.raises(LIFUDeviceError):
        txdevice.load_preset(
            PRESET_INDEX,
            regs_crc(machine_config, 0),
            len(counts),
            0,
            settings_crc=machine_config["settings_crc"],
        )
    # A refused load leaves nothing loaded, so the trigger must not start.
    assert not loaded(txdevice)["loaded"]
    with pytest.raises(LIFUDeviceError):
        txdevice.start_trigger()
