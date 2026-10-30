#!/usr/bin/env bash
# Save migration test: 1.5.0 saves (written by the 1.5.0 core from git history) must load in the current build with
# every unlock, tank, ammo, map, home screen and custom tank intact, compared by name. Usage: tests/save_migration/run.sh [cases]
set -euo pipefail
cd "$(dirname "$0")/../.."
N=${1:-200}
T=build/savemig; rm -rf "$T"; mkdir -p "$T/old"
git archive 03a67f1 core | tar -x -C "$T/old"
CF="-std=c11 -O1 -ffp-contract=off -w"
gcc $CF -I"$T/old/core" -o "$T/writer" tests/save_migration/roundtrip.c "$T"/old/core/*.c "$T/old/core/gen/content_data.c" -lm
gcc $CF -Icore -o "$T/reader" tests/save_migration/roundtrip.c core/*.c core/gen/content_data.c core/gen/legacy_v1.c -lm
fail=0; extra=0
for i in $(seq 1 "$N"); do
  "$T/writer" write "$i" "$T/s.sav" > "$T/a.txt"
  "$T/reader" read "$T/s.sav" > "$T/b.txt" || { echo "case $i: $(cat "$T/b.txt")"; fail=$((fail+1)); continue; }
  # everything the 1.5.0 save held must be there; the new build may add unlocks that 2.0 grants for the same progress
  if ! diff <(grep -v '^granted' "$T/a.txt") <(grep -v '^granted' "$T/b.txt") > "$T/d.txt"; then
    echo "case $i differs:"; head -5 "$T/d.txt"; fail=$((fail+1)); continue
  fi
  missing=$(comm -23 <(grep '^granted' "$T/a.txt" | sort) <(grep '^granted' "$T/b.txt" | sort) | wc -l)
  added=$(comm -13 <(grep '^granted' "$T/a.txt" | sort) <(grep '^granted' "$T/b.txt" | sort) | wc -l)
  if [ "$missing" -ne 0 ]; then echo "case $i: $missing unlocks lost"; fail=$((fail+1)); fi
  extra=$((extra+added))
done
echo "save migration: $((N-fail))/$N saves identical by name ($extra unlocks added by 2.0 rules across all cases)"
[ "$fail" -eq 0 ]
