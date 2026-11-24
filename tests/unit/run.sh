#!/bin/sh
# Host-side unit tests: build and run each test for the standard build and the
# clinical (FDA_MODE) build, as the firmware builds them (thermal_limits.c and
# preset_policy.c have FDA_MODE code), and the preset slots for a bare-metal
# image, an image under the bootloader and the clinical image (presets.c reads
# flash through PRESETS_HOST_FLASH here). Needs a host C compiler (cc); run
# from the repo root.
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

# The fixture's generated preset (tests/unit/fixtures/fda) is the blob the
# slots are written with; its directory comes first on the include path.
for build in bare-metal slot fda; do
	defs="-DPRESETS_HOST_FLASH"
	[ "$build" = bare-metal ] && defs="$defs -DBARE_METAL"
	[ "$build" = fda ] && defs="$defs -DFDA_MODE"
	cc -Itests/unit/fixtures/fda -Itests/unit/stubs $CFLAGS $defs 		tests/unit/test_preset_slots.c Core/Src/presets.c Core/Src/thermal_limits.c 		-o "$out/test_preset_slots_$build"
	"$out/test_preset_slots_$build"
done
