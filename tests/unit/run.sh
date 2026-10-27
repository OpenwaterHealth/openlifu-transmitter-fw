#!/bin/sh
# Host-side unit tests: build and run each test for the DEV and the FDA build.
# Needs a host C compiler (cc); run from the repo root.
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
