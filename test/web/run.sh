#!/usr/bin/env bash
# Run every test/web/test_*.js under the macOS JavaScriptCore shell, or under
# Node (through test/web/node_jsc_shim.js) where jsc is not available.
#
#   bash test/web/run.sh                    # all files
#   JSC=/path/to/jsc bash test/web/run.sh   # a specific jsc
#   WEB_RUNNER=node bash test/web/run.sh    # force Node even on macOS
#
# Runner order: $JSC, the macOS system jsc, `jsc` on PATH, then `node`.
#
# Each file runs as: $JSC test/web/harness.js test/web/<file>, from the repo root,
# so test files can load("data_main/lib.js"). A file passes only if the runner exits 0
# AND it printed a "N passed, 0 failed" summary (some jsc builds ignore quit()'s code,
# and a file that never reaches done() must not count as a pass).
# Output of each file is kept in test/web/<file>.out (git-ignored).

set -u

cd "$(dirname "$0")/../.." || exit 2

# RUN is the command prefix; each test runs as: "${RUN[@]}" harness.js <file>
RUN=()
if [ "${WEB_RUNNER:-auto}" != node ]; then
    if [ -n "${JSC:-}" ]; then
        [ -x "$JSC" ] || { echo "JSC=$JSC is not executable" >&2; exit 2; }
        RUN=("$JSC")
    elif [ -x /System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc ]; then
        RUN=(/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc)
    elif command -v jsc >/dev/null 2>&1; then
        RUN=("$(command -v jsc)")
    fi
fi
if [ ${#RUN[@]} -eq 0 ]; then
    if command -v node >/dev/null 2>&1; then
        RUN=(node test/web/node_jsc_shim.js)
    else
        echo "neither jsc nor node found (set JSC=/path/to/jsc or install Node)" >&2
        exit 2
    fi
fi
echo "web tests via: ${RUN[*]}"

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
    "${RUN[@]}" test/web/harness.js "$f" > "$out" 2>&1
    rc=$?
    cat "$out"
    summary="$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$out" | tail -n 1)"
    if [ $rc -ne 0 ]; then
        echo "-- FAILED: $f (exit $rc)"
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
