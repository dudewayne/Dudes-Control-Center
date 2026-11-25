#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Runs the Dudes Control Center test suite. Called by "make test".
# Usage: tests/run_tests.sh <build-dir with test binaries>
#
# Every test runs in its own empty work directory under <build-dir>/work and
# never touches the real system (no root, no /run sockets, no real sysfs,
# no ~/.config). Exit code is non-zero if any test fails.

set -u
BUILD="$(cd "$1" && pwd)"
TESTS="$(cd "$(dirname "$0")" && pwd)"
WORK="$BUILD/work"
rm -rf "$WORK"
mkdir -p "$WORK"

# Sanitizers: abort on the first error. Leak checking is off because the GUI
# links GTK, which keeps global allocations alive until exit.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

PASSED=0
FAILED=0
FAILED_NAMES=()

run() {
    local name="$1"; shift
    local dir="$WORK/$name"
    mkdir -p "$dir"
    echo "=== $name"
    if (cd "$dir" && "$@") > "$dir/stdout.txt" 2> "$dir/stderr.txt"; then
        grep -E "^(PASS|FAIL|SKIP)" "$dir/stdout.txt" | sed 's/^/    /'
        PASSED=$((PASSED + 1))
    else
        grep -E "^(PASS|FAIL|SKIP)" "$dir/stdout.txt" | sed 's/^/    /'
        echo "    --- $name FAILED; last lines of stdout/stderr:"
        tail -n 15 "$dir/stdout.txt" | sed 's/^/    | /'
        tail -n 30 "$dir/stderr.txt" | sed 's/^/    ! /'
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
}

run fan_responses   "$BUILD/test_fan_responses"
run fan_safety      "$BUILD/test_fan_safety"
run secure_allow    "$BUILD/test_secure_allow" "$WORK/secure_allow" allow
run secure_deny     "$BUILD/test_secure_deny"  "$WORK/secure_deny"  deny
run paths           "$BUILD/test_paths"
run fan_backend     "$BUILD/test_fan_backend"
run cpu_daemon      python3 "$TESTS/test_cpu_daemon.py" "$WORK/cpu_daemon" "$BUILD/cpud_test"
run kbd_daemon      python3 "$TESTS/test_kbd_daemon.py" "$WORK/kbd_daemon" "$BUILD/kbdd_test" "$BUILD/kbdcli_test"
run gui_save        python3 "$TESTS/test_gui_save.py"   "$WORK/gui_save" "$BUILD/test_gui_save" "$BUILD/cpud_test"

echo
echo "Test suites passed: $PASSED, failed: $FAILED"
if [ "$FAILED" -gt 0 ]; then
    echo "Failed: ${FAILED_NAMES[*]}"
    exit 1
fi
