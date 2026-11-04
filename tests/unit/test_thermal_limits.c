/*
 * Host-side test of Core/Src/thermal_limits.c: the start_C / shutoff_C state
 * that gates START and ends a run, kept apart from "a preset is loaded" (#59).
 *
 * The device state thermal_limits.c is handed -- a preset loaded, a sequence
 * running, a fresh reading and its value -- is what presets.c and
 * if_commands.c pass it on the target. That a raw write unloads the preset
 * and leaves the limits alone shows here as "preset_loaded goes false and
 * the gate is unchanged"; on hardware it is tests/hil/test_thermal_limits.py.
 *
 * Built by tests/unit/run.sh with and without -DFDA_MODE. Each test starts
 * from a reset, since the module keeps its state in a static, as RAM does.
 */

#include "common.h"
#include "thermal_limits.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                \
	do {                                                                           \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
			failures++;                                                            \
		}                                                                          \
	} while (0)

// start_C 40.0, shutoff_C 43.0, as a preset's i16 tenths.
#define START_X10   400
#define SHUTOFF_X10 430

static ThermalLimits armed(void)
{
	ThermalLimits l;
	thermal_limits_get(&l);
	return l;
}

static void test_start_check_with_limits(bool loaded)
{
	// At start_C starts; a tenth above does not; a stale reading never does.
	CHECK(thermal_limits_start_check(loaded, true, 40.0f) == OW_SUCCESS);
	CHECK(thermal_limits_start_check(loaded, true, 40.1f) == OW_TEMP_TOO_HIGH);
	CHECK(thermal_limits_start_check(loaded, false, 20.0f) == OW_TEMP_UNKNOWN);
}

static void test_trip_with_limits(void)
{
	CHECK(!thermal_limits_trip(true, 42.9f));
	CHECK(thermal_limits_trip(true, 43.0f));
	CHECK(thermal_limits_trip(false, 20.0f));
}

static void test_load_arms_and_outlasts_the_load(void)
{
	thermal_limits_arm_preset(3, START_X10, SHUTOFF_X10);
	ThermalLimits l = armed();
	CHECK(l.source == THERMAL_LIMITS_PRESET);
	CHECK(l.preset_index == 3);
	CHECK(l.start_c_x10 == START_X10 && l.shutoff_c_x10 == SHUTOFF_X10);

	test_start_check_with_limits(true);
	test_trip_with_limits();

	// A raw write (or an unload) leaves the preset unloaded: the gate holds.
#ifdef FDA_MODE
	// ...except that a presets-only image does not start without a load.
	CHECK(thermal_limits_start_check(false, true, 20.0f) == OW_NO_PRESET);
#else
	test_start_check_with_limits(false);
#endif
	test_trip_with_limits();
	CHECK(armed().source == THERMAL_LIMITS_PRESET);

	// Another load replaces them.
	thermal_limits_arm_preset(4, 300, 350);
	l = armed();
	CHECK(l.preset_index == 4 && l.start_c_x10 == 300 && l.shutoff_c_x10 == 350);
}

#ifdef FDA_MODE

static void test_fda_needs_a_load(void)
{
	// Before any load: none armed, and START is refused whatever the reading.
	CHECK(armed().source == THERMAL_LIMITS_NONE);
	CHECK(armed().preset_index == 0xFF);
	CHECK(thermal_limits_start_check(false, true, 20.0f) == OW_NO_PRESET);
	CHECK(thermal_limits_start_check(true, true, 20.0f) == OW_NO_PRESET);
	CHECK(!thermal_limits_trip(true, 100.0f));
}

#else

// What a reset leaves: none armed. Only a standard image has CLEAR; an FDA
// image only ever arms, so its tests run in an order that needs no reset.
static void reset(void)
{
	CHECK(thermal_limits_clear(false, false) == OW_SUCCESS);
}

static void test_none_armed_runs_ungated(void)
{
	reset();
	ThermalLimits l = armed();
	CHECK(l.source == THERMAL_LIMITS_NONE && l.preset_index == 0xFF);
	CHECK(l.start_c_x10 == 0 && l.shutoff_c_x10 == 0);
	// Research use with nothing armed: only the global TX_OVERHEAT applies.
	CHECK(thermal_limits_start_check(false, false, 100.0f) == OW_SUCCESS);
	CHECK(!thermal_limits_trip(false, 100.0f));
}

static void test_host_set_and_clear(void)
{
	reset();
	CHECK(thermal_limits_set(START_X10, SHUTOFF_X10, false, false) == OW_SUCCESS);
	ThermalLimits l = armed();
	CHECK(l.source == THERMAL_LIMITS_HOST && l.preset_index == 0xFF);
	CHECK(l.start_c_x10 == START_X10 && l.shutoff_c_x10 == SHUTOFF_X10);
	test_start_check_with_limits(false);
	test_trip_with_limits();

	CHECK(thermal_limits_clear(false, false) == OW_SUCCESS);
	CHECK(armed().source == THERMAL_LIMITS_NONE);
	CHECK(thermal_limits_start_check(false, true, 100.0f) == OW_SUCCESS);
	// Clearing with none armed is not an error.
	CHECK(thermal_limits_clear(false, false) == OW_SUCCESS);

	// Negative limits are tenths too.
	CHECK(thermal_limits_set(-50, 10, false, false) == OW_SUCCESS);
	CHECK(thermal_limits_start_check(false, true, -5.0f) == OW_SUCCESS);
	CHECK(thermal_limits_start_check(false, true, -4.9f) == OW_TEMP_TOO_HIGH);
}

static void test_host_set_refusals_change_nothing(void)
{
	reset();
	CHECK(thermal_limits_set(START_X10, SHUTOFF_X10, false, false) == OW_SUCCESS);

	// start_C must be below shutoff_C.
	CHECK(thermal_limits_set(430, 430, false, false) == OW_INVALID_PACKET);
	CHECK(thermal_limits_set(450, 430, false, false) == OW_INVALID_PACKET);
	// Not mid-run, and not over a loaded preset's own limits.
	CHECK(thermal_limits_set(300, 350, false, true) == OW_PRESET_BUSY);
	CHECK(thermal_limits_set(300, 350, true, false) == OW_PRESET_BUSY);
	CHECK(thermal_limits_clear(false, true) == OW_PRESET_BUSY);
	CHECK(thermal_limits_clear(true, false) == OW_PRESET_BUSY);

	ThermalLimits l = armed();
	CHECK(l.source == THERMAL_LIMITS_HOST);
	CHECK(l.start_c_x10 == START_X10 && l.shutoff_c_x10 == SHUTOFF_X10);
}

static void test_preset_limits_need_an_unload_to_change(void)
{
	reset();
	thermal_limits_arm_preset(0, START_X10, SHUTOFF_X10);
	CHECK(thermal_limits_set(300, 350, true, false) == OW_PRESET_BUSY);
	CHECK(thermal_limits_clear(true, false) == OW_PRESET_BUSY);
	CHECK(armed().source == THERMAL_LIMITS_PRESET);
	// Once unloaded (a raw write, or OW_PRESET_LOAD with no payload), the host may.
	CHECK(thermal_limits_set(300, 350, false, false) == OW_SUCCESS);
	CHECK(armed().source == THERMAL_LIMITS_HOST);
	// And a load arms the preset's over the host's.
	thermal_limits_arm_preset(1, START_X10, SHUTOFF_X10);
	CHECK(armed().source == THERMAL_LIMITS_PRESET && armed().preset_index == 1);
}

#endif /* FDA_MODE */

int main(void)
{
#ifdef FDA_MODE
	const char *build = "FDA";
	test_fda_needs_a_load();          // first: it needs nothing armed yet
	test_load_arms_and_outlasts_the_load();
#else
	const char *build = "standard";
	test_none_armed_runs_ungated();
	reset();
	test_load_arms_and_outlasts_the_load();
	test_host_set_and_clear();
	test_host_set_refusals_change_nothing();
	test_preset_limits_need_an_unload_to_change();
#endif
	printf("%s build: thermal limits, %d failure(s)\n", build, failures);
	return failures ? 1 : 0;
}
