#include "thermal_limits.h"

#include "common.h"

#define NO_PRESET_INDEX 0xFFu

static ThermalLimits limits = { THERMAL_LIMITS_NONE, NO_PRESET_INDEX, 0, 0, 0 };

void thermal_limits_arm_preset(uint8_t preset_index, int16_t start_c_x10, int16_t shutoff_c_x10,
                               uint16_t treatment_timeout_s)
{
	limits.source = THERMAL_LIMITS_PRESET;
	limits.preset_index = preset_index;
	limits.start_c_x10 = start_c_x10;
	limits.shutoff_c_x10 = shutoff_c_x10;
	limits.treatment_timeout_s = treatment_timeout_s;
}

void thermal_limits_get(ThermalLimits *out)
{
	*out = limits;
}

#ifndef FDA_MODE

uint8_t thermal_limits_set(int16_t start_c_x10, int16_t shutoff_c_x10,
                           uint16_t treatment_timeout_s, bool running)
{
	if (running) return OW_PRESET_BUSY;
	if (start_c_x10 >= shutoff_c_x10) return OW_INVALID_PACKET;
	limits.source = THERMAL_LIMITS_HOST;
	limits.preset_index = NO_PRESET_INDEX;
	limits.start_c_x10 = start_c_x10;
	limits.shutoff_c_x10 = shutoff_c_x10;
	limits.treatment_timeout_s = treatment_timeout_s;
	return OW_SUCCESS;
}

uint8_t thermal_limits_clear(bool running)
{
	if (running) return OW_PRESET_BUSY;
	limits.source = THERMAL_LIMITS_NONE;
	limits.preset_index = NO_PRESET_INDEX;
	limits.start_c_x10 = 0;
	limits.shutoff_c_x10 = 0;
	limits.treatment_timeout_s = 0;
	return OW_SUCCESS;
}

#endif /* FDA_MODE */

uint8_t thermal_limits_start_check(bool preset_loaded, bool fresh, float temp_c)
{
#ifdef FDA_MODE
	// A presets-only image starts only what it loaded, under that load's limits.
	if (!preset_loaded || limits.source == THERMAL_LIMITS_NONE) return OW_NO_PRESET;
#else
	(void)preset_loaded;
	// None armed: the host's own configuration runs without a thermal gate.
	if (limits.source == THERMAL_LIMITS_NONE) return OW_SUCCESS;
#endif
	if (!fresh) return OW_TEMP_UNKNOWN;
	if (temp_c * 10.0f > (float)limits.start_c_x10) return OW_TEMP_TOO_HIGH;
	return OW_SUCCESS;
}

uint8_t thermal_limits_shutoff_check(bool fresh, float temp_c)
{
	if (limits.source == THERMAL_LIMITS_NONE) return OW_SUCCESS;
	if (!fresh) return OW_TEMP_UNKNOWN;
	if (temp_c * 10.0f >= (float)limits.shutoff_c_x10) return OW_TEMP_TOO_HIGH;
	return OW_SUCCESS;
}

bool thermal_limits_timed_out(uint32_t elapsed_ms)
{
	return limits.treatment_timeout_s != 0U &&
	       elapsed_ms >= (uint32_t)limits.treatment_timeout_s * 1000U;
}
