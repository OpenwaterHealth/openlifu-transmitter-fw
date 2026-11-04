#!/bin/sh
# Host-side unit tests: build and run each test for the standard and the
# presets-only (FDA_MODE) build, as the firmware builds them (preset_policy.c
# has no FDA_MODE code today; thermal_limits.c does). Needs a host C
# compiler (cc); run from the repo root.
set -eu

out=build/unit
mkdir -p "$out"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Itests/unit -ICore/Inc"

for variant in standard fda; do
	defs=""
	[ "$variant" = fda ] && defs="-DFDA_MODE"
	cc $CFLAGS $defs tests/unit/test_preset_policy.c Core/Src/preset_policy.c -o "$out/test_preset_policy_$variant"
	"$out/test_preset_policy_$variant"
	cc $CFLAGS $defs tests/unit/test_thermal_limits.c Core/Src/thermal_limits.c -o "$out/test_thermal_limits_$variant"
	"$out/test_thermal_limits_$variant"
done
