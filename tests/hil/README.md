# Hardware-in-the-loop tests

Host-side pytest checks that run against a transmitter on USB through the
openlifu SDK. They need hardware, so CI does not run them. Record the output as
bench evidence in the PR or issue they verify.

```
pip install openlifu-sdk pytest
pytest tests/hil -v --preset path/to/<preset>.json
```

`--preset` is the preset `.json` the flashed image's preset 0 was generated
from: for an FDA image, the file passed to the SDK's `generate_presets.py`.
Without it the preset tests skip; without a transmitter on USB, every test skips.

Keep the console HV off. These tests load presets and send trigger commands,
and none of them needs acoustic output.

| File | Image | Bench step | Checks |
|---|---|---|---|
| `test_fda_refusals.py` | `Release-FDA` (+fda or +test); skips on a standard image | 4.06 | `DFU` refused as `CMD` and `CONTROLLER` at the master and module 1, and as `ONE_WIRE`, each with `reserved` 0 and 0x77; `I2C_PASSTHRU` to the module bootloader (0x72) refused; the board still answers as the FDA image after each; `RESET` still works. Needs no `--preset`. |
| `test_preset_store.py` | bare-metal standard image (`Release-BareMetal`); skips on +fda / +test, and under the bootloader | 4.00b | stores, loads and deletes a preset in a slot; a raw register or trigger write unloads it, or is refused with `OW_PRESET_BUSY` while it runs; with nothing loaded the host's own trigger config starts; `OW_PRESET_LOADED` across modules; `OW_PRESET_LOCK` (0x56) is gone |
| `test_preset_refusals.py` | `Release-FDA` (+fda or +test); skips on a standard image | 4.00a | `SET_SWTRIG` refused with or without a loaded preset; each run length selected through `OW_PRESET_LOAD`; an out-of-range run length refused |

Bench step 4.06 alone:

```
pytest tests/hil/test_fda_refusals.py -v
```

`test_fda_refusals.py` ends by resetting the transmitter, then reconnects.
