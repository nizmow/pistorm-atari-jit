#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=5.052
commit=ea338be98e1e838d3518809ce8899f85a009963c
export VERILATOR_ROOT=${CPLD_VERILATOR_ROOT:-$repo/.tools/verilator-$version}
build_dir=${CPLD_TEST_BUILD_DIR:-$repo/build/cpld-tests}
jobs=${CPLD_BUILD_JOBS:-2}

if [[ ! -x "$VERILATOR_ROOT/bin/verilator_bin" ]]; then
    for tool in git make autoconf flex bison perl python3 "${CXX:-c++}"; do
        command -v "$tool" >/dev/null || { echo "Missing prerequisite: $tool" >&2; exit 1; }
    done
    if [[ ! -d "$VERILATOR_ROOT" ]]; then
        mkdir -p "$(dirname -- "$VERILATOR_ROOT")"
        git clone --depth 1 --branch "v$version" https://github.com/verilator/verilator.git "$VERILATOR_ROOT"
    fi
    actual=$(git -C "$VERILATOR_ROOT" rev-parse HEAD)
    [[ "$actual" == "$commit" ]] || { echo "Unexpected Verilator revision: $actual" >&2; exit 1; }
    (cd "$VERILATOR_ROOT" && autoconf && ./configure && make -C src -j "$jobs" opt)
fi
[[ $(git -C "$VERILATOR_ROOT" rev-parse HEAD) == "$commit" ]] || {
    echo "Expected Verilator source revision $commit" >&2; exit 1;
}
"$VERILATOR_ROOT/bin/verilator" --version
cmake -S "$repo/tests/cpld" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure "$@"
