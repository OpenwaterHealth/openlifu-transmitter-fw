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
Without it, or without a transmitter on USB, every test is skipped.

Keep the console HV off. These tests load presets and send trigger commands,
and none of them needs acoustic output.

| File | Image | Checks |
|---|---|---|
| `test_preset_refusals.py` | `Release-FDA` (or `Release-Preset` with the preset in slot 0) | `SET_SWTRIG` refused with or without a loaded preset; each run length selected through `OW_PRESET_LOAD`; an out-of-range run length refused |
