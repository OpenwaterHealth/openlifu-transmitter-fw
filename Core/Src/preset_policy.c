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

// The global and controller commands the clinical image refuses.
static uint8_t command_refusal(uint8_t command, uint8_t reserved)
{
	switch (command) {
	case OW_CMD_USR_CFG:
		// reserved 1 is the write; the calibration stays readable.
		return (reserved == 1U) ? OW_FDA_REFUSED : OW_SUCCESS;
	case OW_CMD_DFU:
		// The signed bootloader's DFU only, not the ROM loader: the
		// clinical firmware updater installs signed firmware and presets.
		return (reserved == PRESET_POLICY_DFU_ROM_LOADER) ? OW_FDA_REFUSED : OW_SUCCESS;
	case OW_PRESET_WRITE:
	case OW_PRESET_COMMIT:
	case OW_PRESET_DELETE:
		// The presets come through the bootloader, signed (presets.h).
		return OW_FDA_REFUSED;
	case OW_CTRL_SET_THERMAL_LIMITS:
	case OW_CTRL_CLEAR_THERMAL_LIMITS:
		// The limits are the loaded preset's (thermal_limits.h).
		return OW_FDA_REFUSED;
	default:
		return OW_SUCCESS;
	}
}

uint8_t preset_policy_fda_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved,
                                  bool to_module_bootloader)
{
	if (preset_policy_raw_write(packet_type, command)) return OW_INVALID_PACKET;

	switch (packet_type) {
	case OW_I2C_PASSTHRU:
		// Raw I2C reaches the module bootloaders only, to update the modules
		// (each takes only images signed with its key).
		return to_module_bootloader ? OW_SUCCESS : OW_FDA_REFUSED;
	case OW_CMD:
	case OW_CONTROLLER:
	case OW_ONE_WIRE:
		return command_refusal(command, reserved);
	default:
		return OW_SUCCESS;
	}
}
