#!/bin/sh
# Host-side unit tests: build and run each test as the presets-only (FDA_MODE)
# build, the only one that refuses anything. Needs a host C compiler (cc); run
# from the repo root.
set -eu

out=build/unit
mkdir -p "$out"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Itests/unit -ICore/Inc"

cc $CFLAGS -DFDA_MODE tests/unit/test_preset_policy.c Core/Src/preset_policy.c -o "$out/test_preset_policy"
"$out/test_preset_policy"
