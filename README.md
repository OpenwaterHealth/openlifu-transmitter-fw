# Openwater Open-LIFU 3.2 TX Module Firmware

## Disclaimer

CAUTION - The system described here has not been evaluated by the FDA and is not designed for the treatment or diagnosis of any disease. It is provided AS-IS, with no warranties. User assumes all liability and responsibility for identifying and mitigating risks associated with using this software.

This repository contains the firmware for the TX Module that controls the TI TX7332 Ultrasound Analog Frontend ICs.

## Releases

Every tag (`X.Y.Z`, `X.Y.Z-rc.N`) builds the **research** firmware and publishes
it as the GitHub release (`build-firmware.yml`): the application signed with the
research key, the production image with the research bootloader, the bare-metal
image and the SBOM. Presets are not part of the firmware: one image per release.

The **clinical** firmware is built only on request, on a tag, and only after
approval (`clinical-build.yml`, the `clinical` environment):

```
gh workflow run clinical-build.yml -R OpenwaterHealth/openlifu-transmitter-fw --ref <tag>
```

It builds the clinical image (`Release-FDA`), signs it with the transmitter key in
Google Cloud KMS, verifies it against the clinical bootloader's key, and stores
`openlifu-transmitter-fw-<tag>-signed.bin`, `-production.bin/.hex` (clinical bootloader + app, no
presets), the SBOM, `SHA256SUMS` and `clinical-build-manifest.json` in
`gs://openwater-firmware-artifacts/openlifu-transmitter-fw/<tag>/` (private, write-once). Nothing
is attached to the GitHub release. The presets are signed and added by the
diathermy application's clinical build.
