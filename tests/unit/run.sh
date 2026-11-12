#!/bin/sh
# Host-side unit tests: build and run each test for the standard build and the
# presets-only (FDA_MODE) +fda and +test builds, as the firmware builds them
# (thermal_limits.c has FDA_MODE code; preset_policy.c has FDA_TEST_IMAGE
# code), and the linked presets for each fixture set. Needs a host C compiler
# (cc); run from the repo root.
set -eu

out=build/unit
mkdir -p "$out"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Itests/unit -ICore/Inc"

for variant in standard fda test; do
	defs=""
	[ "$variant" = fda ] && defs="-DFDA_MODE"
	[ "$variant" = test ] && defs="-DFDA_MODE -DFDA_TEST_IMAGE"
	cc $CFLAGS $defs tests/unit/test_preset_policy.c Core/Src/preset_policy.c -o "$out/test_preset_policy_$variant"
	"$out/test_preset_policy_$variant"
	cc $CFLAGS $defs tests/unit/test_thermal_limits.c Core/Src/thermal_limits.c -o "$out/test_thermal_limits_$variant"
	"$out/test_thermal_limits_$variant"
done

# fda: one preset (fda_preset.h); test: a set of four (preset_table.h). The
# fixture comes first on the include path, ahead of anything in Core/Inc/presets.
for set in fda test; do
	defs="-DFDA_MODE"
	[ "$set" = test ] && defs="$defs -DPRESET_TABLE"
	cc -Itests/unit/fixtures/$set -Itests/unit/stubs $CFLAGS $defs \
		tests/unit/test_presets_linked.c Core/Src/presets.c Core/Src/thermal_limits.c \
		-o "$out/test_presets_linked_$set"
	"$out/test_presets_linked_$set"
done
