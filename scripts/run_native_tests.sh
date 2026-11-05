#!/usr/bin/env bash
# Run the host-side Unity suites (test/test_*/) for env:native.
#
#   bash scripts/run_native_tests.sh                 # every suite
#   bash scripts/run_native_tests.sh test_core ...   # selected suites
#   NATIVE_RUNNER=wsl bash scripts/run_native_tests.sh   # force the WSL path
#
# Runner choice (NATIVE_RUNNER=auto|pio|wsl, default auto):
#   pio  `pio test -e native` - needs a host C/C++ compiler (macOS, Linux, or
#        Windows with MinGW on PATH). This is the reference runner and what CI uses.
#   wsl  Windows without a host compiler: compile each suite with g++ inside WSL
#        using the same flags as env:native (-std=gnu++17 -D UNIT_TEST -I src) and
#        the Unity/ArduinoJson copies PlatformIO installed in .pio/libdeps/native.
#
# Exit status is 0 only if every selected suite compiled, ran, and reported
# "0 Failures". A suite with no Unity summary line counts as failed.

set -u
cd "$(dirname "$0")/.." || exit 2

find_pio() {
    if [ -n "${PIO:-}" ]; then echo "$PIO"; return; fi
    command -v pio 2>/dev/null && return
    for p in "$HOME/.platformio/penv/bin/pio" "$HOME/.platformio/penv/Scripts/pio.exe"; do
        [ -x "$p" ] && { echo "$p"; return; }
    done
}

host_compiler() {
    command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1
}

wsl_compiler() {
    command -v wsl.exe >/dev/null 2>&1 &&
        MSYS_NO_PATHCONV=1 wsl.exe -e sh -c 'command -v g++ >/dev/null && command -v gcc >/dev/null' 2>/dev/null
}

PIO_BIN="$(find_pio)"
RUNNER="${NATIVE_RUNNER:-auto}"
if [ "$RUNNER" = auto ]; then
    if host_compiler; then RUNNER=pio
    elif wsl_compiler; then RUNNER=wsl
    else
        echo "No host C++ compiler and no WSL g++: install g++/clang (or WSL with build-essential)." >&2
        exit 2
    fi
fi

# Suites to run: arguments (with or without the test_ prefix), else every test/test_*/
suites=()
if [ $# -gt 0 ]; then
    for s in "$@"; do
        s="${s#test/}"; s="${s%/}"
        case "$s" in test_*) ;; *) s="test_$s" ;; esac
        [ -f "test/$s/test_main.cpp" ] || { echo "no such suite: test/$s/test_main.cpp" >&2; exit 2; }
        suites+=("$s")
    done
else
    for d in test/test_*/; do
        [ -f "${d}test_main.cpp" ] && suites+=("$(basename "$d")")
    done
fi
[ ${#suites[@]} -gt 0 ] || { echo "no native test suites found"; exit 0; }

if [ "$RUNNER" = pio ]; then
    [ -n "$PIO_BIN" ] || { echo "PlatformIO not found (set PIO=/path/to/pio)" >&2; exit 2; }
    args=()
    for s in "${suites[@]}"; do args+=(-f "$s"); done
    echo "native tests via: $PIO_BIN test -e native ${args[*]}"
    exec "$PIO_BIN" test -e native "${args[@]}"
fi

[ "$RUNNER" = wsl ] || { echo "NATIVE_RUNNER must be auto, pio or wsl" >&2; exit 2; }

# The WSL path needs Unity + ArduinoJson; let PlatformIO fetch the env:native
# lib_deps (this works on Windows, no compiler involved).
LIBDEPS=.pio/libdeps/native
if [ ! -f "$LIBDEPS/Unity/src/unity.c" ] || [ ! -d "$LIBDEPS/ArduinoJson/src" ]; then
    [ -n "$PIO_BIN" ] || { echo "PlatformIO not found and $LIBDEPS is empty" >&2; exit 2; }
    echo "Installing env:native libraries..."
    "$PIO_BIN" pkg install -e native || exit 2
fi
if [ ! -f "$LIBDEPS/Unity/src/unity.c" ]; then
    # Unity is the test framework package, not a lib_dep; it lands in the
    # PlatformIO packages dir. Copy it next to ArduinoJson so one -I path works.
    core="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
    for u in "$core"/packages/tool-unity*/ ; do
        [ -f "${u}src/unity.c" ] && { mkdir -p "$LIBDEPS/Unity"; cp -r "${u}src" "$LIBDEPS/Unity/"; break; }
    done
fi
[ -f "$LIBDEPS/Unity/src/unity.c" ] || { echo "Unity sources not found under $LIBDEPS/Unity" >&2; exit 2; }

if command -v cygpath >/dev/null 2>&1; then WINREPO="$(cygpath -w "$PWD")"; else WINREPO="$PWD"; fi

echo "native tests via WSL g++ (no host compiler): ${suites[*]}"
# Flags mirror env:native in platformio.ini. Objects/binaries go to WSL's /tmp
# (executing from /mnt/c is slow and depends on drvfs metadata settings).
MSYS_NO_PATHCONV=1 wsl.exe --cd "$WINREPO" -e bash -c '
    set -u
    out=/tmp/voyager-native-$$
    mkdir -p "$out"
    trap "rm -rf \"$out\"" EXIT
    L=.pio/libdeps/native
    gcc -c -O0 -g -DUNIT_TEST -I"$L/Unity/src" "$L/Unity/src/unity.c" -o "$out/unity.o" || exit 2
    fail=0
    for s in "$@"; do
        echo "== $s"
        if ! g++ -std=gnu++17 -O0 -g -DUNIT_TEST -Iinclude -Isrc -Itest -I"test/$s" \
                -I"$L/Unity/src" -I"$L/ArduinoJson/src" \
                "test/$s/test_main.cpp" "$out/unity.o" -o "$out/$s"; then
            echo "-- FAILED: $s (compile error)"; fail=$((fail + 1)); continue
        fi
        "$out/$s" > "$out/$s.log" 2>&1; rc=$?
        cat "$out/$s.log"
        summary="$(grep -E "^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored" "$out/$s.log" | tail -n 1)"
        if [ $rc -ne 0 ] || [ -z "$summary" ] || ! echo "$summary" | grep -q " 0 Failures"; then
            echo "-- FAILED: $s (exit $rc${summary:+, $summary})"; fail=$((fail + 1))
        else
            echo "-- PASSED: $s ($summary)"
        fi
    done
    echo
    if [ $fail -ne 0 ]; then echo "native tests: $fail of $# suite(s) failed"; exit 1; fi
    echo "native tests: all $# suite(s) passed"
' voyager-native "${suites[@]}"
