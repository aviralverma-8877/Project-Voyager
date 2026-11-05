#!/usr/bin/env bash
# Regenerate the flashed web UI (data/) from its source (data_main/).
#
#   bash scripts/sync_data.sh             # data_main/ -> data/
#   DEST=/tmp/x bash scripts/sync_data.sh # write somewhere else (dry comparison)
#
# - Every file except script.js, lib.js and config/ is mirrored unchanged, and files
#   that no longer exist in data_main/ are removed from the destination
#   (rsync -a --delete; a cp/find fallback is used where rsync is missing, e.g.
#   Git Bash on Windows).
# - script.js / lib.js are minified with a pinned terser.
# - config/*.json is compacted (no whitespace).
#
# Afterwards `diff -rq data data_main` should list only script.js, lib.js and config/*.

set -eu
cd "$(dirname "$0")/.." || exit 2

SRC=data_main
DEST="${DEST:-data}"
TERSER="${TERSER:-terser@5.31.0}"

PY=""
for p in "${PYTHON:-}" python3 python; do
    # Skip the Windows Store python3 stub, which exists but cannot run.
    if [ -n "$p" ] && "$p" -c "import json" >/dev/null 2>&1; then PY="$p"; break; fi
done
[ -n "$PY" ] || { echo "python3 not found (set PYTHON=...)" >&2; exit 2; }
command -v npx >/dev/null 2>&1 || { echo "npx (Node.js) not found; needed for terser" >&2; exit 2; }

is_excluded() {   # path relative to $SRC / $DEST
    case "$1" in
        config|config/*|*/config|*/config/*) return 0 ;;
        script.js|*/script.js|lib.js|*/lib.js) return 0 ;;
    esac
    return 1
}

echo "Syncing $SRC/ -> $DEST/..."
mkdir -p "$DEST"
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude config/ --exclude script.js --exclude lib.js "$SRC/" "$DEST/"
else
    echo "  (rsync not found, using cp fallback)"
    # --delete: drop files and directories that are gone from the source.
    (cd "$DEST" && find . -mindepth 1 -depth -print) | while IFS= read -r rel; do
        rel="${rel#./}"
        is_excluded "$rel" && continue
        if [ ! -e "$SRC/$rel" ] || { [ -d "$DEST/$rel" ] && [ ! -d "$SRC/$rel" ]; }; then
            rm -rf "${DEST:?}/$rel"
            echo "  deleted $rel"
        fi
    done
    # -a: copy everything else, keeping timestamps.
    (cd "$SRC" && find . -mindepth 1 -print) | while IFS= read -r rel; do
        rel="${rel#./}"
        is_excluded "$rel" && continue
        if [ -d "$SRC/$rel" ]; then
            mkdir -p "$DEST/$rel"
        elif ! cmp -s "$SRC/$rel" "$DEST/$rel" 2>/dev/null; then
            cp -p "$SRC/$rel" "$DEST/$rel"
        fi
    done
fi

echo "Minifying JavaScript..."
for f in script.js lib.js; do
    if [ -f "$SRC/$f" ]; then
        npx --yes "$TERSER" "$SRC/$f" -c -m -o "$DEST/$f"
        echo "  $f minified"
    fi
done

echo "Compacting JSON configs..."
mkdir -p "$DEST/config"
for f in "$SRC"/config/*.json; do
    [ -f "$f" ] || continue
    "$PY" -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1])),separators=(',',':')),end='')" \
        "$f" > "$DEST/config/$(basename "$f")"
    echo "  $(basename "$f") compacted"
done
echo "Sync complete!"
