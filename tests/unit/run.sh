#!/bin/sh
# Host-side unit tests: build and run each test for the DEV and the FDA build,
# and the linked presets for each fixture set. Needs a host C compiler (cc);
# run from the repo root.
set -eu

out=build/unit
mkdir -p "$out"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Itests/unit -ICore/Inc"

for variant in dev fda; do
	defs=""
	[ "$variant" = fda ] && defs="-DFDA_MODE"
	cc $CFLAGS $defs tests/unit/test_preset_policy.c Core/Src/preset_policy.c -o "$out/test_preset_policy_$variant"
	"$out/test_preset_policy_$variant"
done

# fda: one preset (fda_preset.h); test: a set of four (preset_table.h). The
# fixture comes first on the include path, ahead of anything in Core/Inc/presets.
for set in fda test; do
	defs="-DFDA_MODE"
	[ "$set" = test ] && defs="$defs -DPRESET_TABLE"
	cc -Itests/unit/fixtures/$set -Itests/unit/stubs $CFLAGS $defs \
		tests/unit/test_presets_linked.c Core/Src/presets.c -o "$out/test_presets_linked_$set"
	"$out/test_presets_linked_$set"
done
