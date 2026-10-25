#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$root" \
    "$root/tests/monster/test_monster.c" "$root/platforms/atari/monster.c" \
    -o "$build/registers"
"$build/registers"
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -I"$root" -I"$root/gpio" \
    -I"$root/include" -I"$root/threaddep" \
    "$root/tests/monster/test_config.c" "$root/config_file/config_file.c" \
    -o "$build/config"
"$build/config"
if [ "$#" -gt 0 ]; then
    ${CC:-cc} -std=gnu11 -O1 -I"$root" -I"$root/third_party/musashi" \
        "$root/tests/monster/test_driver.c" "$root/platforms/atari/monster.c" \
        "$root/third_party/musashi/m68kcpu.c" \
        "$root/third_party/musashi/m68kops.c" \
        "$root/third_party/musashi/softfloat/softfloat.c" -lm -o "$build/driver"
    "$build/driver" "$1"
fi
