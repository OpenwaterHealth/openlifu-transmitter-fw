# Hardware-in-the-loop tests

Host-side pytest checks that run against a transmitter on USB through the
openlifu SDK. They need hardware, so CI does not run them. Record the output as
bench evidence in the PR or issue they verify.

```
pip install openlifu-sdk pytest
pytest tests/hil -v --rootdir . --preset path/to/<preset>.json
pytest tests/hil -v --rootdir . --context path/to/preset_settings/<context>
```

Name the presets the flashed image holds, one way or the other:

- `--preset`: the preset `.json` the image's preset 0 was generated from. For a
  `+fda` image, that is the `fda` context's one preset.
- `--context`: a preset context folder, as in openlifu-diathermy-application's
  `preset_settings/<context>` (`presets.json`, then `<id>/<id>_settings.json`):
  the whole set a presets-only image links, in index order. For a `+test` image,
  `preset_settings/test`.

Without either, the preset tests skip; without a transmitter on USB, every test
skips. `--rootdir .` keeps pytest from taking the rootdir from the preset path.

A presets-only (`Release-FDA*`) image reports its set as a version suffix,
`+fda` or `+test`. A DEV image reports no suffix.

Keep the console HV off. These tests load presets and send trigger commands,
and none of them needs acoustic output.

| File | Image | Bench step | Checks |
|---|---|---|---|
| `test_fda_refusals.py` | presets-only, `+fda` or `+test` (skips on DEV) | 4.06, 4.08b | `DFU` refused as `CMD` and `CONTROLLER` at the master and module 1, and as `ONE_WIRE`, each with `reserved` 0 and 0x77; `I2C_PASSTHRU` to the module bootloader (0x72) refused; the board still answers with the same suffix after each; `RESET` still works. Needs no `--preset`. |
| `test_linked_presets.py` | presets-only, `+fda` or `+test` (skips on DEV) | 4.04, 4.04b, 4.08b | the image links exactly the set given (`OW_PRESET_GET` count); each index is the host's preset of that index and loads; each index refuses a wrong `settings_crc` and every other index's CRCs; no index past the set (count, count + 1, 255). With `--context`, the suffix names the context. |
| `test_preset_lock.py` | presets-only, or DEV (bare-metal, to store the preset) | 4.00a, 4.00b | DEV unlocked: raw writes go through and unload; the lock turns on and stays on; locked refuses what a presets-only image refuses; reads stay open; Start needs a preset |
| `test_preset_refusals.py` | presets-only, or a locked DEV image | 4.00a, 4.00b | `SET_SWTRIG` refused with or without a loaded preset; each run length of preset 0 selected through `OW_PRESET_LOAD`; an out-of-range run length refused |

Bench step 4.06 alone:

```
pytest tests/hil/test_fda_refusals.py -v
```

Bench step 4.08b, on a `+test` image:

```
pytest tests/hil -v --rootdir . --context path/to/preset_settings/test
```

`test_fda_refusals.py` ends by resetting the transmitter, then reconnects.
