/*
 * Host-side test of the preset lock's refusal table (Core/Src/preset_policy.c).
 *
 * Every (packet type, command, reserved) a host can send is checked against
 * the table below, which is written out by hand: the FDA build's refusals
 * must be exactly those of the FDA image before the DEV build existed (#40,
 * #34), and the DEV build's locked refusals the same plus storing presets.
 *
 * Built twice by tests/unit/run.sh, with and without -DFDA_MODE.
 * Pass --print to list the refusals instead.
 */

#include "common.h"
#include "preset_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

// The global commands, which the controller and the one-wire bus both take.
static uint8_t expected_global_refusal(uint8_t command, uint8_t reserved)
{
	switch (command) {
	case OW_CMD_USR_CFG:
		return (reserved == 1U) ? OW_FDA_REFUSED : OW_SUCCESS;
	case OW_CMD_DFU:
		return OW_FDA_REFUSED;
#ifndef FDA_MODE
	case OW_PRESET_WRITE:
	case OW_PRESET_COMMIT:
	case OW_PRESET_DELETE:
		return OW_FDA_REFUSED;
#endif
	default:
		return OW_SUCCESS;
	}
}

// What the lock must refuse, and with which code; OW_SUCCESS otherwise.
static uint8_t expected_refusal(uint8_t packet_type, uint8_t command, uint8_t reserved)
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
			return OW_INVALID_PACKET;
		default:
			return OW_SUCCESS;
		}
	case OW_CMD:
	case OW_CONTROLLER:
		switch (command) {
		case OW_CTRL_SET_DELAY_PROFILE:
		case OW_CTRL_SET_PATTERN_PROFILE:
		case OW_CTRL_SET_PROFILE_CYCLE:
		case OW_CTRL_SET_SWTRIG:
			return OW_INVALID_PACKET;
		default:
			return expected_global_refusal(command, reserved);
		}
	case OW_ONE_WIRE:
		return expected_global_refusal(command, reserved);
	case OW_I2C_PASSTHRU:
		return OW_FDA_REFUSED;
	default:
		return OW_SUCCESS;
	}
}

static const uint8_t packet_types[] = {
	OW_ACK, OW_NAK, OW_CMD, OW_RESP, OW_DATA, OW_ONE_WIRE, OW_TX7332, OW_AFE_READ,
	OW_AFE_SEND, OW_I2C_PASSTHRU, OW_CONTROLLER, OW_POWER, OW_ONEWIRE_RESP, OW_TIMEOUT, OW_ERROR,
};
#define N_TYPES (sizeof(packet_types) / sizeof(packet_types[0]))

static void check(const char *what, unsigned got, unsigned want, uint8_t pt, uint8_t cmd, uint8_t res)
{
	if (got != want) {
		printf("FAIL %s: type 0x%02X cmd 0x%02X reserved %u: got 0x%02X, want 0x%02X\n",
		       what, pt, cmd, res, got, want);
		failures++;
	}
}

int main(int argc, char **argv)
{
	int print = (argc > 1 && strcmp(argv[1], "--print") == 0);
	unsigned refused = 0, raw = 0;

	for (unsigned t = 0; t < N_TYPES; t++) {
		for (unsigned c = 0; c < 256U; c++) {
			// reserved only matters to USR_CFG (0 read, 1 write); 2 and 0xFF stand in for the rest.
			static const uint8_t reserveds[] = { 0U, 1U, 2U, 0xFFU };
			for (unsigned r = 0; r < sizeof(reserveds); r++) {
				uint8_t pt = packet_types[t], cmd = (uint8_t)c, res = reserveds[r];
				uint8_t got = preset_policy_locked_refusal(pt, cmd, res);
				uint8_t want = expected_refusal(pt, cmd, res);
				check("locked refusal", got, want, pt, cmd, res);
				if (got != OW_SUCCESS) {
					refused++;
					if (print && r < 2U) printf("refuse type 0x%02X cmd 0x%02X reserved %u -> 0x%02X\n", pt, cmd, res, got);
				}
				if (r == 0U) {
					// Raw writes are exactly the OW_INVALID_PACKET refusals, whatever reserved is.
					bool is_raw = preset_policy_raw_write(pt, cmd);
					check("raw write", is_raw, want == OW_INVALID_PACKET, pt, cmd, res);
					raw += is_raw ? 1U : 0U;
				}
			}
		}
	}

	// Spot checks that read as the requirement, not the table.
	check("SET_SWTRIG refused", preset_policy_locked_refusal(OW_CONTROLLER, OW_CTRL_SET_SWTRIG, 0), OW_INVALID_PACKET, OW_CONTROLLER, OW_CTRL_SET_SWTRIG, 0);
	check("GET_SWTRIG open", preset_policy_locked_refusal(OW_CONTROLLER, OW_CTRL_GET_SWTRIG, 0), OW_SUCCESS, OW_CONTROLLER, OW_CTRL_GET_SWTRIG, 0);
	check("START open", preset_policy_locked_refusal(OW_CONTROLLER, OW_CTRL_START_SWTRIG, 0), OW_SUCCESS, OW_CONTROLLER, OW_CTRL_START_SWTRIG, 0);
	check("STOP open", preset_policy_locked_refusal(OW_CONTROLLER, OW_CTRL_STOP_SWTRIG, 0), OW_SUCCESS, OW_CONTROLLER, OW_CTRL_STOP_SWTRIG, 0);
	check("PRESET_LOAD open", preset_policy_locked_refusal(OW_CONTROLLER, OW_PRESET_LOAD, 0), OW_SUCCESS, OW_CONTROLLER, OW_PRESET_LOAD, 0);
	check("PRESET_LOADED open", preset_policy_locked_refusal(OW_CONTROLLER, OW_PRESET_LOADED, 0), OW_SUCCESS, OW_CONTROLLER, OW_PRESET_LOADED, 0);
	check("PRESET_LOCK open", preset_policy_locked_refusal(OW_CONTROLLER, OW_PRESET_LOCK, 1), OW_SUCCESS, OW_CONTROLLER, OW_PRESET_LOCK, 1);
	check("USR_CFG read open", preset_policy_locked_refusal(OW_CMD, OW_CMD_USR_CFG, 0), OW_SUCCESS, OW_CMD, OW_CMD_USR_CFG, 0);
	check("TX7332 read open", preset_policy_locked_refusal(OW_TX7332, OW_TX7332_RREG, 0), OW_SUCCESS, OW_TX7332, OW_TX7332_RREG, 0);
	check("ARM_PROFILE_CYCLE not a host write", preset_policy_raw_write(OW_CONTROLLER, OW_CTRL_ARM_PROFILE_CYCLE), 0, OW_CONTROLLER, OW_CTRL_ARM_PROFILE_CYCLE, 0);

#ifdef FDA_MODE
	const char *build = "FDA";
#else
	const char *build = "DEV";
#endif
	printf("%s build: %u refused (type, command, reserved) combinations, %u raw-write (type, command) pairs, %d failure(s)\n",
	       build, refused, raw, failures);
	return failures ? 1 : 0;
}
