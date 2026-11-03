/*
 * preset_policy.h
 *
 * Which host commands a presets-only image (FDA_MODE) refuses. Pure: no HAL,
 * no device state, so the table can be tested on the host (tests/unit). A
 * standard image refuses none of them.
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
 * A presets-only image refuses them: they belong to the loaded preset.
 */
bool preset_policy_raw_write(uint8_t packet_type, uint8_t command);

/**
 * @brief  Does a presets-only image refuse this command?
 * @return OW_SUCCESS if it allows it, else the OW_ERROR reply's code:
 *         OW_INVALID_PACKET for a raw write, OW_FDA_REFUSED for the rest
 *         (user config write, DFU, raw I2C).
 */
uint8_t preset_policy_fda_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved);

#endif /* INC_PRESET_POLICY_H_ */
