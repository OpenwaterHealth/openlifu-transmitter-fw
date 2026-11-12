"""A standard image's thermal limits: their own state, apart from the load (#59).

start_C, shutoff_C and the treatment timeout are armed by a load or by the
host, and stay armed through a raw write or an unload. A host set or clear unloads a loaded preset,
as a raw write does. Armed, they gate START and end a run whether or not a
preset is loaded. Needs a bare-metal standard image to store the
preset in slot 0 (conftest.py); skipped on +fda / +test, where
test_preset_refusals.py checks that the host cannot set or clear them.

Keep the console HV off. test_shutoff_stops_a_run needs the TX to warm by a
few tenths of a degree with HV off: it waits up to --shutoff-wait seconds
(default 300); warming the transducer by hand speeds it up.
"""

import json
import math
import time

import pytest
from conftest import (
    OW_CTRL_PAUSE_SWTRIG,
    OW_CTRL_RESUME_SWTRIG,
    PRESET_INDEX,
    armed_limits,
    clear_thermal_limits,
    decode_thermal_limits,
    loaded,
    send_raw,
    set_thermal_limits,
)
from openlifu_sdk.io.exceptions import LIFUDeviceError
from openlifu_sdk.io.LIFUConfig import (
    OW_CONTROLLER,
    OW_CTRL_SET_SWTRIG,
    OW_ERROR,
    OW_INVALID_PACKET,
    OW_PRESET_BUSY,
    OW_TEMP_TOO_HIGH,
)
from openlifu_sdk.io.LIFUTXPresets import load_preset, thermal_limits, treatment_timeout

# The trigger's TriggerModes (trigger.h): trains that can pause, and a run
# that goes on until stopped.
TRIGGER_MODE_SEQUENCE = 0
TRIGGER_MODE_CONTINUOUS = 1


def _unload_all(txdevice):
    for module in range(txdevice.get_module_count()):
        txdevice.unload_preset(module)
    assert not loaded(txdevice)["loaded"]


def _host_trigger(txdevice, **changes) -> None:
    """Set the host's own trigger config: a raw write, which unloads."""
    # Drop GET_SWTRIG's read-only fields: the firmware ignores them, and with
    # them the payload sits one byte under SET_SWTRIG's 255-byte buffer (#63).
    trigger = {k: v for k, v in {**txdevice.get_trigger_json(), **changes}.items()
               if k not in ("TrainCount", "TriggerStatus")}
    r = send_raw(
        txdevice,
        OW_CONTROLLER,
        OW_CTRL_SET_SWTRIG,
        data=bytearray(json.dumps(trigger).encode("ascii")),
    )
    assert r is not None and r.packet_type != OW_ERROR


def _train_interval(trigger) -> int:
    """An interval that holds the whole train (34ele: 16 pulses at 10 Hz, 1.6 s); START refuses a shorter one."""
    f, n = int(trigger.get("TriggerFrequencyHz", 0)), int(trigger.get("TriggerPulseCount", 0))
    train_us = (1_000_000 // f) * n if f else 0
    return max(int(trigger.get("TriggerPulseTrainInterval", 0)), train_us + 100_000, 500_000)


def _wait_status(txdevice, status: str, wait_s: float) -> bool:
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        if txdevice.get_trigger()["trigger_status"] == status:
            return True
        time.sleep(0.2)
    return False


def _tenth_above(celsius: float, tenths: int) -> float:
    """A limit *tenths* above a reading, on the wire format's 0.1 C grid."""
    return (math.ceil(celsius * 10) + tenths) / 10


@pytest.fixture
def host_mode(txdevice, standard):
    """Nothing loaded and no limits armed, before and after the test."""
    _unload_all(txdevice)
    assert decode_thermal_limits(clear_thermal_limits(txdevice))["source"] == "none"
    yield
    txdevice.stop_trigger()
    _unload_all(txdevice)
    clear_thermal_limits(txdevice)


def test_limits_stay_armed_after_a_raw_write(txdevice, machine_config, preset_in_slot, host_mode):
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    armed = armed_limits(txdevice)
    assert armed["source"] == "preset" and armed["preset_index"] == PRESET_INDEX
    assert armed["x10"] == tuple(thermal_limits(machine_config))
    assert armed["treatment_timeout_s"] == treatment_timeout(machine_config)

    _host_trigger(txdevice)

    assert not loaded(txdevice)["loaded"], "a raw write still unloads"
    assert armed_limits(txdevice) == armed, "and leaves the limits armed"

    # So does an unload.
    _unload_all(txdevice)
    assert armed_limits(txdevice) == armed


def test_host_set_and_clear(txdevice, host_mode):
    r = set_thermal_limits(txdevice, 40.0, 43.0, timeout_s=1800)
    armed = decode_thermal_limits(r)
    assert armed == {
        "source": "host",
        "preset_index": None,
        "x10": (400, 430),
        "start_c": 40.0,
        "shutoff_c": 43.0,
        "treatment_timeout_s": 1800,
    }
    assert armed_limits(txdevice) == armed

    # 0: the thermal limits without a timeout.
    no_timeout = decode_thermal_limits(set_thermal_limits(txdevice, 40.0, 43.0, timeout_s=0))
    assert no_timeout["source"] == "host" and no_timeout["treatment_timeout_s"] == 0

    cleared = decode_thermal_limits(clear_thermal_limits(txdevice))
    assert cleared["source"] == "none" and cleared["x10"] == (0, 0)
    assert cleared["treatment_timeout_s"] == 0
    assert armed_limits(txdevice) == cleared


def test_host_set_refusals_change_nothing(txdevice, machine_config, preset_in_slot, host_mode):
    assert set_thermal_limits(txdevice, 40.0, 43.0).packet_type != OW_ERROR
    before = armed_limits(txdevice)

    for start, shutoff in ((43.0, 43.0), (45.0, 43.0)):
        r = set_thermal_limits(txdevice, start, shutoff)
        assert (r.packet_type, r.reserved) == (OW_ERROR, OW_INVALID_PACKET), "start_C below shutoff_C"
    if txdevice.get_module_count() > 1:
        r = set_thermal_limits(txdevice, 30.0, 35.0, addr=1)
        assert (r.packet_type, r.reserved) == (OW_ERROR, OW_INVALID_PACKET), "the master's limits act"
    assert armed_limits(txdevice) == before


def test_host_set_or_clear_unloads_a_preset(txdevice, machine_config, preset_in_slot, host_mode):
    # Load, then modify: the preset no longer describes what is armed.
    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    assert loaded(txdevice)["loaded"]
    armed = decode_thermal_limits(set_thermal_limits(txdevice, 30.0, 35.0, timeout_s=0))
    assert armed["source"] == "host" and armed["x10"] == (300, 350)
    assert armed["treatment_timeout_s"] == 0, "the preset's timeout goes with its limits"
    assert not loaded(txdevice)["loaded"], "a host set unloads, as a raw write does"

    load_preset(txdevice, PRESET_INDEX, machine_config, duration_index=0)
    assert armed_limits(txdevice)["source"] == "preset"
    assert decode_thermal_limits(clear_thermal_limits(txdevice))["source"] == "none"
    assert not loaded(txdevice)["loaded"], "so does a clear"


def test_start_refused_above_start_c_with_no_preset(txdevice, host_mode):
    _host_trigger(txdevice, TriggerMode=TRIGGER_MODE_CONTINUOUS)
    temp = txdevice.get_temperature()
    # start_C a degree below the reading: refused, with no preset loaded.
    start = _tenth_above(temp, -10)
    assert set_thermal_limits(txdevice, start, start + 20.0).packet_type != OW_ERROR

    with pytest.raises(LIFUDeviceError) as e:
        txdevice.start_trigger()
    assert e.value.device_error_code == OW_TEMP_TOO_HIGH
    assert txdevice.get_trigger()["trigger_status"] != "RUNNING"

    # A start_C above the reading lets it start.
    start = _tenth_above(temp, 20)
    assert set_thermal_limits(txdevice, start, start + 20.0).packet_type != OW_ERROR
    before = armed_limits(txdevice)
    txdevice.start_trigger()
    # The run keeps the limits it started under.
    for r in (set_thermal_limits(txdevice, 30.0, 35.0), set_thermal_limits(txdevice, start, start + 20.0),
              clear_thermal_limits(txdevice)):
        assert (r.packet_type, r.reserved) == (OW_ERROR, OW_PRESET_BUSY)
    assert armed_limits(txdevice) == before
    txdevice.stop_trigger()

    # Cleared, nothing gates it.
    assert clear_thermal_limits(txdevice).packet_type != OW_ERROR
    txdevice.start_trigger()
    txdevice.stop_trigger()


def test_shutoff_stops_a_run(txdevice, request, host_mode):
    wait_s = request.config.getoption("--shutoff-wait")
    _host_trigger(txdevice, TriggerMode=TRIGGER_MODE_CONTINUOUS)
    # start_C just above the reading, so it starts; shutoff_C a tenth above
    # that, so a little warming ends the run.
    start = _tenth_above(txdevice.get_temperature(), 2)
    shutoff = round(start + 0.1, 1)
    assert set_thermal_limits(txdevice, start, shutoff).packet_type != OW_ERROR
    assert not loaded(txdevice)["loaded"]

    txdevice.start_trigger()
    deadline = time.monotonic() + wait_s
    stopped_at = None
    while time.monotonic() < deadline:
        if txdevice.get_trigger()["trigger_status"] != "RUNNING":
            stopped_at = txdevice.get_temperature()
            break
        time.sleep(0.5)

    assert stopped_at is not None, (
        f"still running after {wait_s:.0f} s with shutoff_C {shutoff} C "
        f"(TX {txdevice.get_temperature():.2f} C): warm the transducer, or raise --shutoff-wait"
    )
    # The firmware stops at shutoff_C, or on a reading gone stale.
    assert stopped_at >= shutoff - 0.1, f"stopped at {stopped_at:.2f} C, below shutoff_C {shutoff} C"
    assert armed_limits(txdevice)["source"] == "host", "a trip leaves the limits armed"


def test_limits_refused_while_paused(txdevice, host_mode):
    """A paused sequence resumes on the limits and timeout it started under."""
    trigger = txdevice.get_trigger_json()
    _host_trigger(txdevice, TriggerMode=TRIGGER_MODE_SEQUENCE, TriggerPulseTrainCount=1000,
                  TriggerPulseTrainInterval=_train_interval(trigger))
    start = _tenth_above(txdevice.get_temperature(), 20)
    assert set_thermal_limits(txdevice, start, start + 20.0, timeout_s=600).packet_type != OW_ERROR
    before = armed_limits(txdevice)

    txdevice.start_trigger()
    r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_PAUSE_SWTRIG)
    assert r is not None and r.packet_type != OW_ERROR
    assert _wait_status(txdevice, "PAUSED", 5.0), "the pause lands at the next train boundary"
    for r in (set_thermal_limits(txdevice, start, start + 20.0, timeout_s=0), clear_thermal_limits(txdevice)):
        assert (r.packet_type, r.reserved) == (OW_ERROR, OW_PRESET_BUSY)
    assert armed_limits(txdevice) == before


def test_resume_refused_at_shutoff_c_with_temp_too_high(txdevice, request, host_mode):
    """A paused sequence at or above the armed shutoff_C stays paused, refused
    with OW_TEMP_TOO_HIGH, the code START gives above start_C. Waits up to
    --shutoff-wait s for the TX to warm a tenth or two with HV off."""
    wait_s = request.config.getoption("--shutoff-wait")
    trigger = txdevice.get_trigger_json()
    _host_trigger(txdevice, TriggerMode=TRIGGER_MODE_SEQUENCE, TriggerPulseTrainCount=1000,
                  TriggerPulseTrainInterval=_train_interval(trigger))
    start = _tenth_above(txdevice.get_temperature(), 1)
    shutoff = round(start + 0.1, 1)
    assert set_thermal_limits(txdevice, start, shutoff, timeout_s=0).packet_type != OW_ERROR

    txdevice.start_trigger()
    r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_PAUSE_SWTRIG)
    assert r is not None and r.packet_type != OW_ERROR
    assert _wait_status(txdevice, "PAUSED", 5.0), "the pause lands at the next train boundary"

    deadline = time.monotonic() + wait_s
    while txdevice.get_temperature() < shutoff and time.monotonic() < deadline:
        time.sleep(0.5)
    temp = txdevice.get_temperature()
    if temp < shutoff:
        pytest.fail(f"TX {temp:.2f} C never reached shutoff_C {shutoff} C in {wait_s:.0f} s: "
                    "warm the transducer, or raise --shutoff-wait")
    r = send_raw(txdevice, OW_CONTROLLER, OW_CTRL_RESUME_SWTRIG)
    assert r is not None and (r.packet_type, r.reserved) == (OW_ERROR, OW_TEMP_TOO_HIGH)
    assert txdevice.get_trigger()["trigger_status"] == "PAUSED", "the pause stays for a later resume"


def test_host_timeout_ends_a_run_with_no_preset(txdevice, host_mode):
    _host_trigger(txdevice, TriggerMode=TRIGGER_MODE_CONTINUOUS)
    start = _tenth_above(txdevice.get_temperature(), 20)
    assert set_thermal_limits(txdevice, start, start + 20.0, timeout_s=2).packet_type != OW_ERROR
    assert not loaded(txdevice)["loaded"]

    txdevice.start_trigger()
    assert _wait_status(txdevice, "TIMEOUT", 5.0), "a 2 s treatment timeout ends the run"
    assert armed_limits(txdevice)["treatment_timeout_s"] == 2, "and stays armed"
    txdevice.stop_trigger()

    # 0: no timeout, so the run outlasts 2 s.
    assert set_thermal_limits(txdevice, start, start + 20.0, timeout_s=0).packet_type != OW_ERROR
    txdevice.start_trigger()
    time.sleep(3.0)
    assert txdevice.get_trigger()["trigger_status"] == "RUNNING"
    txdevice.stop_trigger()
