"""The clinical image (FDA_MODE) keeps its firmware and presets to what the
bootloader takes signed (SR-021).

Clinical image only: on a standard image these commands would run, so every
test here skips there. Keep the console HV off.

It enters the signed bootloader's DFU (reserved 0) and lets raw I2C reach a
module bootloader (0x72, or a module the master enumerated in its bootloader),
so the clinical firmware updater can update it. DFU entry is not run here,
since it leaves a board in its bootloader; the I2C gate is checked at 0x72
with no bootloader listening. It refuses the STM32 ROM loader (0x77), raw I2C
anywhere else, and storing or deleting a preset itself (those come signed,
through the bootloader).

The last test resets the transmitter and reconnects.
"""

import struct
import time

import pytest
from conftest import send_raw
from openlifu_sdk.io.LIFUConfig import (
    OW_CMD,
    OW_CMD_DFU,
    OW_CMD_RESET,
    OW_CONTROLLER,
    OW_ERROR,
    OW_FDA_REFUSED,
    OW_I2C_PASSTHRU,
    OW_ONE_WIRE,
    OW_PRESET_COMMIT,
    OW_PRESET_DELETE,
    OW_PRESET_WRITE,
)

# The module DFU bootloader's I2C address (openlifu_sdk.io.LIFUDFU).
I2C_DFU_SLAVE_ADDR = 0x72
# OW_I2C_PASSTHRU command 0x01: write, then read back `reserved` bytes.
PASSTHRU_WRITE_READ = 0x01
# STM32 I2C bootloader "Get" and its complement: a read even if it ran.
BOOTLOADER_GET = b"\x00\xff"

# OW_CMD_DFU reserved: enter the STM32 ROM loader, refused by the clinical image.
DFU_ROM_LOADER = 0x77
# Not a module bootloader's I2C address: not 0x72, and not a module (0x20 on).
I2C_OTHER_ADDR = 0x50

RECONNECT_TIMEOUT_S = 20.0


@pytest.fixture(autouse=True)
def fda_only(variant):
    if variant != "fda":
        pytest.skip("the clinical (FDA_MODE) image only: a standard image would run these")


@pytest.fixture(scope="module")
def version(txdevice):
    return txdevice.get_version()


def _assert_refused_and_still_running(txdevice, version, r):
    assert r is not None, "no reply"
    assert (r.packet_type, r.reserved) == (OW_ERROR, OW_FDA_REFUSED)
    # Still the application, not a bootloader.
    assert txdevice.ping()
    assert txdevice.get_version() == version


@pytest.mark.parametrize("addr", [0, 1], ids=["master", "module1"])
@pytest.mark.parametrize("packet_type", [OW_CMD, OW_CONTROLLER], ids=["CMD", "CONTROLLER"])
def test_rom_loader_dfu_is_refused(txdevice, version, packet_type, addr):
    r = send_raw(txdevice, packet_type, OW_CMD_DFU, addr=addr, reserved=DFU_ROM_LOADER)
    _assert_refused_and_still_running(txdevice, version, r)


def test_one_wire_rom_loader_dfu_is_refused(txdevice, version):
    r = send_raw(txdevice, OW_ONE_WIRE, OW_CMD_DFU, reserved=DFU_ROM_LOADER)
    _assert_refused_and_still_running(txdevice, version, r)


@pytest.mark.parametrize("command, data", [
    (OW_PRESET_WRITE, struct.pack("<H", 0) + b"PRS1"),
    (OW_PRESET_COMMIT, struct.pack("<II", 4, 0)),
    (OW_PRESET_DELETE, b""),
], ids=["write", "commit", "delete"])
def test_storing_or_deleting_a_preset_is_refused(txdevice, version, command, data):
    r = send_raw(txdevice, OW_CONTROLLER, command, reserved=0, data=bytearray(data))
    _assert_refused_and_still_running(txdevice, version, r)


def test_raw_i2c_reaches_the_module_bootloader(txdevice):
    """The gate lets it through. No module bootloader listens at 0x72 here, so
    the I2C write fails, but not with the FDA refusal."""
    r = send_raw(
        txdevice,
        OW_I2C_PASSTHRU,
        PASSTHRU_WRITE_READ,
        addr=I2C_DFU_SLAVE_ADDR,
        reserved=1,
        data=bytearray(BOOTLOADER_GET),
    )
    assert r is not None, "no reply"
    assert not (r.packet_type == OW_ERROR and r.reserved == OW_FDA_REFUSED), "refused by the FDA gate"
    assert txdevice.ping()


def test_i2c_passthrough_elsewhere_is_refused(txdevice, version):
    r = send_raw(
        txdevice,
        OW_I2C_PASSTHRU,
        PASSTHRU_WRITE_READ,
        addr=I2C_OTHER_ADDR,
        reserved=1,
        data=bytearray(b"\x00"),
    )
    _assert_refused_and_still_running(txdevice, version, r)


def test_reset_still_works_after_the_refusals(txdevice, version):
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
    assert txdevice.get_version() == version, "did not come back as the same image"
