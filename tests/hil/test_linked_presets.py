"""A presets-only image links exactly the preset set it names (SR-003).

Bench steps 4.00a / 4.04 (+fda, one preset) and 4.04b / 4.08b (+test, a set).
Give the set the image was built from:

    pytest tests/hil/test_linked_presets.py --context path/to/preset_settings/test

Each index is the host's preset of that index, loads only with its own CRCs,
and the image has no index past the set. Presets-only images only; a DEV
image's slots are covered by test_preset_lock.py. Keep the console HV off:
nothing here starts the trigger.
"""

from pathlib import Path

import pytest
from conftest import PRESETS_ONLY, loaded
from openlifu_sdk.io.exceptions import LIFUDeviceError
from openlifu_sdk.io.LIFUConfig import OW_BAD_CRC, OW_INVALID_PACKET
from openlifu_sdk.io.LIFUTXPresets import (
    load_preset,
    regs_crc,
    train_count_selections,
    verify_preset,
)

# The firmware takes the index as a u8; the last one is past any set.
LAST_INDEX = 255


@pytest.fixture(autouse=True)
def presets_only(variant):
    if variant not in PRESETS_ONLY:
        pytest.skip("presets-only (+fda / +test) images only")


def _load_refusal(txdevice, index, machine_config):
    """Load *index* with *machine_config*'s CRCs: the device's refusal code, or
    None if it loaded."""
    try:
        txdevice.load_preset(
            index, regs_crc(machine_config, 0), 0, 0, settings_crc=machine_config["settings_crc"]
        )
    except LIFUDeviceError as e:
        return e.device_error_code
    return None


def test_the_suffix_names_the_set(request, variant):
    context = request.config.getoption("--context")
    if context is None:
        pytest.skip("no --context given")
    assert Path(context).name == variant, f"the image reports +{variant}"


def test_the_image_links_the_whole_set(txdevice, preset_set):
    assert txdevice.get_preset(0)["count"] == len(preset_set)


def test_each_index_is_the_hosts_preset(txdevice, preset_set, preset_index):
    mc = preset_set[preset_index]
    verify_preset(txdevice, preset_index, mc)

    load_preset(txdevice, preset_index, mc, duration_index=0)

    counts = train_count_selections(mc)
    assert txdevice.get_trigger_json()["TriggerPulseTrainCount"] == counts[0]
    state = loaded(txdevice)
    assert state["loaded"] and state["missing"] == 0
    assert (state["index"], state["duration_index"]) == (preset_index, 0)
    assert state["train_count"] == counts[0]
    assert state["settings_crc"] == mc["settings_crc"]
    assert state["regs_crc"] == regs_crc(mc, 0)


def test_an_index_loads_only_with_its_own_crcs(txdevice, preset_set, preset_index):
    mc = preset_set[preset_index]
    wrong_settings = dict(mc, settings_crc=mc["settings_crc"] ^ 1)
    assert _load_refusal(txdevice, preset_index, wrong_settings) == OW_BAD_CRC
    for j, other in enumerate(preset_set):
        if j != preset_index:
            code = _load_refusal(txdevice, preset_index, other)
            assert code == OW_BAD_CRC, f"index {j}'s CRCs: {code}"
    # A refused load leaves nothing loaded.
    assert not loaded(txdevice)["loaded"]


@pytest.mark.parametrize("past", [0, 1, None], ids=["count", "count+1", "255"])
def test_no_index_past_the_set(txdevice, preset_set, past):
    index = LAST_INDEX if past is None else len(preset_set) + past
    with pytest.raises(LIFUDeviceError) as e:
        txdevice.get_preset(index)
    assert e.value.device_error_code == OW_INVALID_PACKET
    assert _load_refusal(txdevice, index, preset_set[0]) is not None
    assert not loaded(txdevice)["loaded"]
