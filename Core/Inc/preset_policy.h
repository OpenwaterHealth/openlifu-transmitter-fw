/*
 * preset_policy.h
 *
 * Which host commands the preset lock refuses, and which ones undermine a
 * loaded preset. Pure: no HAL, no device state, so the table can be tested
 * on the host (tests/unit).
 *
 * The lock is always on in FDA_MODE. In the DEV build the host turns it on
 * with OW_PRESET_LOCK, and only a reset turns it off (presets.h).
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
 * Locked, these are refused. Unlocked, they go through and leave nothing
 * loaded, so the device never reports a preset the chips no longer hold.
 */
bool preset_policy_raw_write(uint8_t packet_type, uint8_t command);

/**
 * @brief  Would the lock refuse this command?
 * @return OW_SUCCESS if the lock allows it, else the OW_ERROR reply's code:
 *         OW_INVALID_PACKET for a raw write, OW_FDA_REFUSED for the rest
 *         (user config write, DFU, raw I2C, and in the DEV build storing or
 *         deleting a preset).
 */
uint8_t preset_policy_locked_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved);

#endif /* INC_PRESET_POLICY_H_ */
