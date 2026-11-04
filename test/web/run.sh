#!/usr/bin/env bash
# Run every test/web/test_*.js under the macOS JavaScriptCore shell.
#
#   bash test/web/run.sh            # all files
#   JSC=/path/to/jsc bash test/web/run.sh
#
# Each file runs as: $JSC test/web/harness.js test/web/<file>, from the repo root,
# so test files can load("data_main/lib.js"). A file passes only if jsc exits 0
# AND it printed a "N passed, 0 failed" summary (jsc may ignore quit()'s code,
# and a file that never reaches done() must not count as a pass).
# Output of each file is kept in test/web/<file>.out (git-ignored).

set -u

cd "$(dirname "$0")/../.." || exit 2

JSC="${JSC:-/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc}"
if [ ! -x "$JSC" ]; then
    echo "jsc not found at $JSC (set JSC=/path/to/jsc)" >&2
    exit 2
fi

shopt -s nullglob
files=(test/web/test_*.js)
if [ ${#files[@]} -eq 0 ]; then
    echo "no web tests"
    exit 0
fi

failed_files=0
for f in "${files[@]}"; do
    out="${f%.js}.out"
    echo "== $f"
    "$JSC" test/web/harness.js "$f" > "$out" 2>&1
    rc=$?
    cat "$out"
    summary="$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$out" | tail -n 1)"
    if [ $rc -ne 0 ]; then
        echo "-- FAILED: $f (jsc exit $rc)"
        failed_files=$((failed_files + 1))
    elif [ -z "$summary" ]; then
        echo "-- FAILED: $f (no summary line; did it call done()?)"
        failed_files=$((failed_files + 1))
    elif [ "${summary##*, }" != "0 failed" ]; then
        echo "-- FAILED: $f ($summary)"
        failed_files=$((failed_files + 1))
    fi
done

echo
if [ $failed_files -ne 0 ]; then
    echo "web tests: $failed_files of ${#files[@]} file(s) failed"
    exit 1
fi
echo "web tests: all ${#files[@]} file(s) passed"
exit 0
