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
`+fda` or `+test`. A standard image reports no suffix.

Keep the console HV off. These tests load presets and send trigger commands,
and none of them needs acoustic output.

`test_preset_store.py` deletes every preset it stored, but a run cut short can
leave pages 110-125 written. Erase them (or mass-erase) before flashing the
bootloader and a signed image on that board: bootloader 1.0.1 will not boot
an image whose slot holds anything there (#56).

| File | Image | Bench step | Checks |
|---|---|---|---|
| `test_fda_refusals.py` | presets-only, `+fda` or `+test`; skips on a standard image | 4.06, 4.08b | `DFU` refused as `CMD` and `CONTROLLER` at the master and module 1, and as `ONE_WIRE`, each with `reserved` 0 and 0x77; `I2C_PASSTHRU` to the module bootloader (0x72) refused; the board still answers with the same suffix after each; `RESET` still works. Needs no `--preset`. |
| `test_linked_presets.py` | presets-only, `+fda` or `+test`; skips on a standard image | 4.04, 4.04b, 4.08b | the image links exactly the set given (`OW_PRESET_GET` count); each index is the host's preset of that index and loads; each index refuses a wrong `settings_crc` and every other index's CRCs; no index past the set (count, count + 1, 255). With `--context`, the suffix names the context. |
| `test_preset_store.py` | bare-metal standard image (`Release-BareMetal`); skips on +fda / +test, and under the bootloader | 4.00b | stores, loads and deletes a preset in a slot; a raw register or trigger write unloads it, or is refused with `OW_PRESET_BUSY` while it runs; with nothing loaded the host's own trigger config starts; `OW_PRESET_LOADED` across modules; `OW_PRESET_LOCK` (0x56) is gone |
| `test_thermal_limits.py` | bare-metal standard image, as `test_preset_store.py` | — | `start_C` / `shutoff_C` and the treatment timeout stay armed after a raw write or an unload; host set and clear (timeout 0 = none), and their refusal while running or paused (pause sent raw); a host set or clear unloads a loaded preset; a host timeout of 2 s ends a run with no preset loaded, and 0 lets it run on; a paused sequence at `shutoff_C` refuses RESUME with `OW_TEMP_TOO_HIGH` (waits up to `--shutoff-wait` for the TX to warm); START refused above `start_C` with no preset loaded; `shutoff_C` stops a run with no preset loaded (waits up to `--shutoff-wait` s, default 300, for the TX to warm a few tenths) |
| `test_preset_refusals.py` | presets-only, `+fda` or `+test`; skips on a standard image | 4.00a | `SET_SWTRIG` refused with or without a loaded preset; the thermal limits and treatment timeout are the loaded preset's, and setting or clearing them is refused; each run length of preset 0 selected through `OW_PRESET_LOAD`; an out-of-range run length refused |

Bench step 4.06 alone:

```
pytest tests/hil/test_fda_refusals.py -v
```

Bench step 4.08b, on a `+test` image:

```
pytest tests/hil -v --rootdir . --context path/to/preset_settings/test
```

`test_fda_refusals.py` ends by resetting the transmitter, then reconnects.
