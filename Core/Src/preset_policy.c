#include "preset_policy.h"

#include "common.h"

bool preset_policy_raw_write(uint8_t packet_type, uint8_t command)
{
	switch (packet_type) {
	case OW_TX7332:
		switch (command) {
		case OW_TX7332_WREG:
		case OW_TX7332_WBLOCK:
		case OW_TX7332_VWREG:
		case OW_TX7332_VWBLOCK:
		case OW_TX7332_RESET:
		case OW_TX7332_DEMO:
			return true;
		default:
			return false;
		}
	case OW_CMD:
	case OW_CONTROLLER:
		switch (command) {
		case OW_CTRL_SET_DELAY_PROFILE:
		case OW_CTRL_SET_PATTERN_PROFILE:
		case OW_CTRL_SET_PROFILE_CYCLE:
		case OW_CTRL_SET_SWTRIG:
			return true;
		default:
			return false;
		}
	default:
		return false;
	}
}

uint8_t preset_policy_locked_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved)
{
	if (preset_policy_raw_write(packet_type, command)) return OW_INVALID_PACKET;

	switch (packet_type) {
	case OW_I2C_PASSTHRU:
		// Raw I2C reaches the module bootloaders (SR-021).
		return OW_FDA_REFUSED;
	case OW_CMD:
	case OW_CONTROLLER:
	case OW_ONE_WIRE:
		switch (command) {
		case OW_CMD_USR_CFG:
			// reserved 1 is the write; the calibration stays readable.
			return (reserved == 1U) ? OW_FDA_REFUSED : OW_SUCCESS;
		case OW_CMD_DFU:
			return OW_FDA_REFUSED;
#ifndef FDA_MODE
		// The FDA image has no stored presets to change.
		case OW_PRESET_WRITE:
		case OW_PRESET_COMMIT:
		case OW_PRESET_DELETE:
			return OW_FDA_REFUSED;
#endif
		default:
			return OW_SUCCESS;
		}
	default:
		return OW_SUCCESS;
	}
}
