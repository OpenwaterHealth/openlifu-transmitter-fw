"""A presets-only image refuses DFU entry and raw I2C to the module bootloaders (SR-021).

Bench steps 4.06 (+fda) and 4.08b (+test). Presets-only images only: on a DEV
image these commands would run, and DFU would drop the board into its
bootloader, so every test here skips there.
The locked-DEV equivalents are in test_preset_lock.py. Keep the console HV off.

The last test resets the transmitter and reconnects.
"""

import time

import pytest
from conftest import PRESETS_ONLY, send_raw
from openlifu_sdk.io.LIFUConfig import (
    OW_CMD,
    OW_CMD_DFU,
    OW_CMD_RESET,
    OW_CONTROLLER,
    OW_ERROR,
    OW_FDA_REFUSED,
    OW_I2C_PASSTHRU,
    OW_ONE_WIRE,
)

# The module DFU bootloader's I2C address (openlifu_sdk.io.LIFUDFU).
I2C_DFU_SLAVE_ADDR = 0x72
# OW_I2C_PASSTHRU command 0x01: write, then read back `reserved` bytes.
PASSTHRU_WRITE_READ = 0x01
# STM32 I2C bootloader "Get" and its complement: a read even if it ran.
BOOTLOADER_GET = b"\x00\xff"

RECONNECT_TIMEOUT_S = 20.0


@pytest.fixture(autouse=True)
def presets_only(variant):
    if variant not in PRESETS_ONLY:
        pytest.skip("presets-only (+fda / +test) images only: a DEV image would run these")


def _assert_refused_and_still_running(txdevice, variant, r):
    assert r is not None, "no reply"
    assert (r.packet_type, r.reserved) == (OW_ERROR, OW_FDA_REFUSED)
    # Still the application, not a bootloader.
    assert txdevice.ping()
    assert txdevice.get_version().endswith("+" + variant)


@pytest.mark.parametrize("reserved", [0x00, 0x77])
@pytest.mark.parametrize("addr", [0, 1], ids=["master", "module1"])
@pytest.mark.parametrize("packet_type", [OW_CMD, OW_CONTROLLER], ids=["CMD", "CONTROLLER"])
def test_dfu_is_refused(txdevice, variant, packet_type, addr, reserved):
    r = send_raw(txdevice, packet_type, OW_CMD_DFU, addr=addr, reserved=reserved)
    _assert_refused_and_still_running(txdevice, variant, r)


@pytest.mark.parametrize("reserved", [0x00, 0x77])
def test_one_wire_dfu_is_refused(txdevice, variant, reserved):
    r = send_raw(txdevice, OW_ONE_WIRE, OW_CMD_DFU, reserved=reserved)
    _assert_refused_and_still_running(txdevice, variant, r)


def test_i2c_passthrough_to_the_module_bootloader_is_refused(txdevice, variant):
    r = send_raw(
        txdevice,
        OW_I2C_PASSTHRU,
        PASSTHRU_WRITE_READ,
        addr=I2C_DFU_SLAVE_ADDR,
        reserved=1,
        data=bytearray(BOOTLOADER_GET),
    )
    _assert_refused_and_still_running(txdevice, variant, r)


def test_reset_still_works_after_the_refusals(txdevice, variant):
    r = send_raw(txdevice, OW_CMD, OW_CMD_RESET)
    assert r is not None and r.packet_type != OW_ERROR, "reset refused"

    txdevice.close()
    # The firmware resets ~1.5 s after replying; don't reconnect to it before then.
    time.sleep(3.0)
    deadline = time.monotonic() + RECONNECT_TIMEOUT_S
    while not txdevice.connect():
        assert time.monotonic() < deadline, "the transmitter did not come back after reset"
        time.sleep(0.5)

    assert txdevice.ping()
    assert txdevice.get_version().endswith("+" + variant), f"did not come back as +{variant}"
