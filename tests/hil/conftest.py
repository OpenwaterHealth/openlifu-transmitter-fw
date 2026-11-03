"""Hardware-in-the-loop fixtures: a transmitter on USB and the preset its image holds.

Run from the repo root with the openlifu SDK installed:

    pytest tests/hil --preset path/to/<preset>.json

Skipped unless --preset is given and a transmitter answers on USB.
"""

import json
from pathlib import Path

import pytest
from openlifu_sdk.io.LIFUTXDevice import LIFUTxDevice
from openlifu_sdk.io.LIFUTXPresets import compile_preset


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
    return compile_preset(json.loads(raw), settings_bytes=raw)


@pytest.fixture(scope="session")
def txdevice():
    # The research class, so the test can put commands on the wire that the
    # FDA TxDevice would refuse to send: the point is that the firmware refuses.
    tx = LIFUTxDevice()
    if not tx.connect():
        pytest.skip("no transmitter on USB")
    yield tx
    tx.close()
