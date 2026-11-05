/*
 * thermal_limits.h
 *
 * The TX thermistor limits that gate START and end a running sequence:
 *   start_C    the trigger does not start above it, or without a fresh reading
 *   shutoff_C  a running sequence stops at it, or when the reading goes stale
 *
 * Their own state, not part of "a preset is loaded" (#59). OW_PRESET_LOAD arms
 * them from the preset. A raw register, profile or trigger write unloads the
 * preset (presets_raw_write()) and leaves them armed, as do an unload and a
 * refused load: only another successful load, the host's SET or CLEAR, or a
 * reset changes them. They are RAM state, so every reset starts with none.
 * A SET or CLEAR also unloads a loaded preset, as a raw write does: the
 * preset's limits are part of what it was checked as, so the device must not
 * report it loaded under other limits (if_commands.c).
 *
 *   FDA_MODE  only a load arms them, and START needs a load as well
 *             (OW_NO_PRESET). SET and CLEAR are refused (preset_policy.c)
 *             and not built in.
 *   standard  the host may also SET its own limits, or CLEAR them, while no
 *             sequence runs: research use without a preset, or a loaded
 *             preset changed after the load. With none armed, START runs the
 *             host's configuration with only the global TX_OVERHEAT
 *             protection.
 *
 * Only the master runs the trigger, so the master's limits are the ones that
 * act; the commands address module 0 only. A slave keeps what its own loads
 * armed, which nothing reads.
 *
 * Pure: the caller passes in the device state (loaded, running, the reading)
 * and does any unload, so the logic can be tested on the host (tests/unit).
 */

#ifndef INC_THERMAL_LIMITS_H_
#define INC_THERMAL_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>

// Where the armed limits came from (OW_CTRL_GET_THERMAL_LIMITS reply byte 0).
typedef enum {
	THERMAL_LIMITS_NONE = 0,     // none armed
	THERMAL_LIMITS_PRESET = 1,   // the last preset this module loaded
	THERMAL_LIMITS_HOST = 2,     // OW_CTRL_SET_THERMAL_LIMITS (standard image only)
} ThermalLimitsSource;

typedef struct {
	uint8_t source;              // ThermalLimitsSource
	uint8_t preset_index;        // the preset that armed them; 0xFF unless source is PRESET
	int16_t start_c_x10;         // tenths of a degree C; 0 when none are armed
	int16_t shutoff_c_x10;
} ThermalLimits;

/** A preset loaded: arm its limits, whatever was armed before. */
void thermal_limits_arm_preset(uint8_t preset_index, int16_t start_c_x10, int16_t shutoff_c_x10);

/** What is armed now. */
void thermal_limits_get(ThermalLimits *out);

#ifndef FDA_MODE

/**
 * @brief  Arm the host's own limits, over whatever is armed. The caller
 *         unloads a loaded preset first.
 * @return OW_SUCCESS; OW_INVALID_PACKET unless start_C is below shutoff_C;
 *         OW_PRESET_BUSY while a sequence runs or is paused (running). A
 *         refusal changes nothing.
 */
uint8_t thermal_limits_set(int16_t start_c_x10, int16_t shutoff_c_x10, bool running);

/**
 * @brief  Disarm the limits. Nothing armed is not an error. The caller
 *         unloads a loaded preset first.
 * @return OW_SUCCESS, or OW_PRESET_BUSY, changing nothing, while a sequence
 *         runs or is paused (running).
 */
uint8_t thermal_limits_clear(bool running);

#endif /* FDA_MODE */

/**
 * @brief  May the trigger start now?
 * @param  preset_loaded  a preset is loaded on this module.
 * @param  fresh          the TX reading is recent enough to act on.
 * @param  temp_c         the TX reading, degrees C.
 * @return OW_SUCCESS, or why not: OW_NO_PRESET (FDA_MODE only),
 *         OW_TEMP_UNKNOWN, OW_TEMP_TOO_HIGH.
 */
uint8_t thermal_limits_start_check(bool preset_loaded, bool fresh, float temp_c);

/** Must a running sequence stop? At or above shutoff_C, or on a stale reading; never with none armed. */
bool thermal_limits_trip(bool fresh, float temp_c);

#endif /* INC_THERMAL_LIMITS_H_ */
