#!/usr/bin/env bash
# tools/vendor_squared_data.sh SQUARED: copy squared's data module (sq.data,
# its JSON facade) into third_party/squared-data/, and record where from.
#
#   tools/vendor_squared_data.sh ../squared/packages/package.squared-core/tree/squared
#
# sqmake reads and writes JSON (tool calls: squared's docs/developer/sdk-tools.md)
# only through sq.data, never yyjson directly, so that what squared does
# with JSON is done once. The copy is committed, so a checkout builds offline and without
# squared beside it; run this again when squared's data module changes, then
# build, test and commit third_party/squared-data/ with its PINNED.
set -euo pipefail
SRC=${1:?"squared's tree (the folder holding data/)"}
[ -f "$SRC/data/module.cppm" ] || { echo "no data/module.cppm under $SRC" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
OUT=$ROOT/third_party/squared-data
mkdir -p "$OUT/src"
find "$OUT" -type f \( -name '*.cppm' -o -name '*.cpp' \) -delete
cp "$SRC"/data/*.cppm "$OUT/"
cp "$SRC"/data/src/*.cppm "$SRC"/data/src/*.cpp "$OUT/src/"
commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)
{
  echo "# squared's data module (sq.data), copied by tools/vendor_squared_data.sh."
  echo "# Do not edit: change it in squared, then copy it again."
  echo "from: squared $commit"
  echo "copied: $(date -u +%Y-%m-%d)"
  (cd "$OUT" && find . -type f \( -name '*.cppm' -o -name '*.cpp' \) | LC_ALL=C sort | xargs sha256sum)
} > "$OUT/PINNED"
cp "$SRC/../../../../LICENSE" "$OUT/LICENSE" 2>/dev/null || true
echo "sq.data copied into third_party/squared-data ($commit)"
