/*
 * preset_policy.h
 *
 * Which host commands a presets-only image (FDA_MODE) refuses, and which
 * ones change what a loaded preset runs. Pure: no HAL, no device state, so
 * the tables can be tested on the host (tests/unit). A standard image runs
 * them all: a raw write there leaves nothing loaded, or is refused with
 * OW_PRESET_BUSY while a loaded preset runs (presets.h).
 */

#ifndef INC_PRESET_POLICY_H_
#define INC_PRESET_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief  Does this command change what a loaded preset runs: the TX7332
 *         registers, the delay or pattern profiles, the profile cycle or the
 *         trigger timing?
 *
 * A presets-only image refuses them: they belong to the loaded preset. A
 * standard image runs them and leaves nothing loaded, so the device never
 * reports a preset the chips no longer hold.
 */
bool preset_policy_raw_write(uint8_t packet_type, uint8_t command);

/** OW_CMD_DFU's reserved value that enters the STM32 ROM loader. */
#define PRESET_POLICY_DFU_ROM_LOADER 0x77U

/**
 * @brief  Does a presets-only image refuse this command?
 *
 * A +fda image refuses DFU and raw I2C outright. A +test image
 * (FDA_TEST_IMAGE) lets through the signed bootloader's DFU and raw I2C to
 * a module bootloader, so a unit on the test presets can be updated over
 * USB; it still refuses the STM32 ROM loader, which takes unsigned images.
 *
 * @param  to_module_bootloader  for OW_I2C_PASSTHRU: the packet's I2C
 *         address is a module bootloader's (the caller knows which modules
 *         are in their bootloader; module_manager.h). Ignored otherwise.
 * @return OW_SUCCESS if it allows it, else the OW_ERROR reply's code:
 *         OW_INVALID_PACKET for a raw write, OW_FDA_REFUSED for the rest
 *         (user config write, DFU, raw I2C, setting or clearing the thermal
 *         limits).
 */
uint8_t preset_policy_fda_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved,
                                  bool to_module_bootloader);

#endif /* INC_PRESET_POLICY_H_ */
