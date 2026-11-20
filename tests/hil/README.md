# Hardware-in-the-loop tests

Host-side pytest checks that run against a transmitter on USB through the
openlifu SDK. They need hardware, so CI does not run them. Record the output as
bench evidence in the PR or issue they verify.

```
pip install openlifu-sdk pytest
pytest tests/hil -v --rootdir . --preset path/to/<preset>.json
pytest tests/hil -v --rootdir . --context path/to/preset_settings/<context>
```

Name the preset the tests use, one way or the other:

- `--preset`: a preset `.json`, the one in slot 0.
- `--context`: a preset context folder, as in openlifu-diathermy-application's
  `preset_settings/<context>` (`presets.json`, then `<id>/<id>_settings.json`);
  its first preset is the one in slot 0.

Without either, the preset tests skip; without a transmitter on USB, every test
skips. `--rootdir .` keeps pytest from taking the rootdir from the preset path.

Every image reads its presets from the preset slots (pages 106-125, 10 slots of
4 KB, each a signed `PRS1` header then the blob). A standard image stores a
preset in slot 0 itself, which needs the slots: a bare-metal image
(`Release-BareMetal`), or one under transmitter bootloader >= 1.1.0-rc.1. The
clinical image (`Release-FDA`) cannot: write this preset into its slot 0
through the bootloader first (openlifu-sdk
`LIFUDFUManager.program_transmitter_preset`). The version carries no variant:
the tests tell the clinical image by its refusal of a preset write.

Keep the console HV off. These tests load presets and send trigger commands,
and none of them needs acoustic output.

`test_preset_store.py` deletes every preset it stored.

| File | Image | Checks |
|---|---|---|
| `test_fda_refusals.py` | clinical; skips on a standard image | `DFU` to the ROM loader (`reserved` 0x77) refused as `CMD` and `CONTROLLER` at the master and module 1, and as `ONE_WIRE`; `OW_PRESET_WRITE` / `COMMIT` / `DELETE` refused; `I2C_PASSTHRU` to the module bootloader (0x72) let through (no bootloader listens, so it fails, but not as refused); `I2C_PASSTHRU` to another address refused; the board answers with the same version after each; `RESET` still works. `DFU` with `reserved` 0 (the signed bootloader's) is allowed and not run. Needs no `--preset`. |
| `test_preset_store.py` | standard, with the preset slots; skips on the clinical image | stores, loads and deletes a preset in a slot; a raw register or trigger write unloads it, or is refused with `OW_PRESET_BUSY` while it runs; with nothing loaded the host's own trigger config starts; `OW_PRESET_LOADED` across modules; `OW_PRESET_LOCK` (0x56) is gone |
| `test_thermal_limits.py` | standard, as `test_preset_store.py` | `start_C` / `shutoff_C` and the treatment timeout stay armed after a raw write or an unload; host set and clear (timeout 0 = none), and their refusal while running or paused (pause sent raw); a host set or clear unloads a loaded preset; a host timeout of 2 s ends a run with no preset loaded, and 0 lets it run on; a paused sequence at `shutoff_C` refuses RESUME with `OW_TEMP_TOO_HIGH` (waits up to `--shutoff-wait` for the TX to warm); START refused above `start_C` with no preset loaded; `shutoff_C` stops a run with no preset loaded (waits up to `--shutoff-wait` s, default 300, for the TX to warm a few tenths) |
| `test_preset_refusals.py` | clinical, with this preset in slot 0; skips on a standard image | `SET_SWTRIG` refused with or without a loaded preset; the thermal limits and treatment timeout are the loaded preset's, and setting or clearing them is refused; each run length of preset 0 selected through `OW_PRESET_LOAD`; an out-of-range run length refused |

The refusals alone:

```
pytest tests/hil/test_fda_refusals.py -v
```

`test_fda_refusals.py` ends by resetting the transmitter, then reconnects.
