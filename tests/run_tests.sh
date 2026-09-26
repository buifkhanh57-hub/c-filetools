#!/bin/sh
# tests/run_tests.sh — end-to-end test suite for diskdive.
#
# Builds a deterministic test tree in /tmp with KNOWN file sizes and
# contents, then asserts on the real output of every subcommand:
#
#   du / top / dups / types / ages / tree / clean (dry run) / report
#   --json variants (validated as JSON) / exit codes / global options
#
# Usage:  ./tests/run_tests.sh   (or: make test)
# Exit:   0 when every check passes, 1 otherwise.

set -u

PROGDIR=$(cd "$(dirname "$0")/.." && pwd)
BIN="$PROGDIR/diskdive"
TT="/tmp/diskdive-test-suite.$$"
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }
note() { printf -- '---  %s\n' "$1"; }

# assert_contains NAME FILE-or-string PATTERN
assert_contains() {
    name=$1; hay=$2; pat=$3
    case "$hay" in
    *"$pat"*) ok "$name" ;;
    *)        bad "$name (missing: $pat)" ;;
    esac
}

# ---- 0. binary present (build on demand) --------------------------
if [ ! -x "$BIN" ]; then
    note "binary missing; running make"
    make -C "$PROGDIR" >/dev/null 2>&1 || { echo "FATAL: cannot build diskdive"; exit 1; }
fi
[ -x "$BIN" ] || { echo "FATAL: $BIN is not executable"; exit 1; }
ok "binary built"

# ---- 1. deterministic test tree -----------------------------------
rm -rf "$TT"
mkdir -p "$TT/project/media" "$TT/project/docs/deep" "$TT/project/build/node_modules" "$TT/project/.cache"

DUPDATA=$(head -c 200 /dev/zero | tr '\0' 'D')
printf '%s' "$DUPDATA" > "$TT/project/dup1.txt"
cp "$TT/project/dup1.txt" "$TT/project/dup2.txt"
cp "$TT/project/dup1.txt" "$TT/project/docs/deep/dup3.txt"

head -c 1048576 /dev/zero | tr '\0' 'A' > "$TT/project/media/big.bin"   # 1 MiB
head -c 2048     /dev/zero | tr '\0' 'P' > "$TT/project/media/photo.jpg"
head -c 10       /dev/zero | tr '\0' 'T' > "$TT/project/junk.tmp"       # old *.tmp
head -c 40       /dev/zero | tr '\0' 'L' > "$TT/project/notes.log"
head -c 8        /dev/zero | tr '\0' 'J' > "$TT/project/build/node_modules/junk.js"
head -c 10       /dev/zero | tr '\0' 'C' > "$TT/project/.cache/blob.bin"
printf 'deep file!\n'                 > "$TT/project/docs/deep/deep.txt" # 11 B
printf '{"key": "value", "n": 42}\n'  > "$TT/project/data.json"          # 26 B
touch -t 202001010000 "$TT/project/junk.tmp"                             # aged

# expected apparent total, computed from the tree itself
EXPECT_TOTAL=$(find "$TT/project" -type f -exec wc -c {} + | tail -1 | awk '{print $1}')
note "test tree ready ($EXPECT_TOTAL apparent bytes expected)"

# ---- 2. du --------------------------------------------------------
out=$("$BIN" du "$TT/project" --apparent --bytes --top 0 2>&1)
assert_contains "du exits 0 and prints header" "$out" "Disk usage for"
total=$(printf '%s\n' "$out" | sed -n 's/^Total: \([0-9,]*\).*/\1/p' | tr -d ',')
if [ "$total" = "$EXPECT_TOTAL" ]; then
    ok "du apparent total = $EXPECT_TOTAL"
else
    bad "du total: got '$total', want '$EXPECT_TOTAL'"
fi
assert_contains "du row for media dir" "$out" "project/media"
assert_contains "du nested dir visible" "$out" "project/docs/deep"

out=$("$BIN" du "$TT/project" -d1 2>&1)
case "$out" in
    *docs/deep*) bad "du --depth 1 hides level-2 rows" ;;
    *media*)     ok "du --depth 1 keeps level-1 rows"  ;;
    *)           bad "du --depth 1 output"             ;;
esac

# ---- 3. top -------------------------------------------------------
out=$("$BIN" top "$TT/project" -n 3 --bytes 2>&1)
assert_contains "top ranks big.bin first" "$out" "1,048,576"
assert_contains "top names big.bin"       "$out" "media/big.bin"
assert_contains "top footer counts files" "$out" "11 files scanned"

# ---- 4. dups ------------------------------------------------------
out=$("$BIN" dups "$TT/project" 2>&1)
assert_contains "dups finds the 3 identical files" "$out" "Group 1: 3 files"
assert_contains "dups lists dup1" "$out" "project/dup1.txt"
assert_contains "dups lists nested dup3" "$out" "project/docs/deep/dup3.txt"
assert_contains "dups summary: 2 redundant, 400 B wasted" "$out" \
    "Summary: 1 duplicate group · 2 redundant files · 400 B wasted"

out=$("$BIN" dups "$TT/project" --min-size 1KiB 2>&1)
assert_contains "dups --min-size filters small files" "$out" "No duplicate files found."

# ---- 5. types -----------------------------------------------------
out=$("$BIN" types "$TT/project" --bytes 2>&1)
assert_contains "types aggregates bin"   "$out" "binary"
assert_contains "types counts 4 txt"     "$out" "document"
assert_contains "types total matches"    "$out" "Total: 11 files"

# ---- 6. ages ------------------------------------------------------
out=$("$BIN" ages "$TT/project" 2>&1)
assert_contains "ages has <1 day bucket"    "$out" "<1 day"
assert_contains "ages old .tmp in >1 year"  "$out" ">1 year"
assert_contains "ages highlights oldest"    "$out" "junk.tmp"

# ---- 7. tree ------------------------------------------------------
out=$("$BIN" tree "$TT/project" --ascii 2>&1)
assert_contains "tree draws connectors"    "$out" "|--"
assert_contains "tree shows media dir"     "$out" "media/"
assert_contains "tree shows nested dir"    "$out" "deep/"
case "$out" in
    *".cache"*) bad "tree hides hidden dirs by default" ;;
    *)          ok "tree hides hidden dirs by default"  ;;
esac
out=$("$BIN" tree "$TT/project" --all --depth 1 2>&1)
assert_contains "tree --all shows hidden" "$out" ".cache"

# ---- 8. clean (DRY RUN — must not delete) -------------------------
out=$("$BIN" clean "$TT/project" 2>&1)
assert_contains "clean prints DRY RUN banner" "$out" "DRY RUN"
assert_contains "clean flags *.tmp"           "$out" "junk.tmp"
assert_contains "clean flags *.log"           "$out" "notes.log"
assert_contains "clean flags node_modules"    "$out" "node_modules"
if [ -f "$TT/project/junk.tmp" ] && [ -f "$TT/project/notes.log" ] \
   && [ -f "$TT/project/build/node_modules/junk.js" ]; then
    ok "clean dry run deleted nothing"
else
    bad "clean dry run deleted files!"
fi
out=$("$BIN" clean "$TT/project" --delete 2>&1)
assert_contains "clean --delete refuses without tty/--yes" "$out" "refusing to delete"
[ -f "$TT/project/junk.tmp" ] && ok "refused delete kept files" || bad "refused delete lost files"

# ---- 9. report ----------------------------------------------------
out=$("$BIN" report "$TT/project" 2>&1)
for section in "Overview" "Largest directories" "Largest files" "Extensions" "Age" "Duplicates" "Clean candidates"; do
    assert_contains "report section: $section" "$out" "== $section =="
done
assert_contains "report counts 11 files"     "$out" "11 files"
assert_contains "report dup summary"         "$out" "2 redundant files"

# ---- 10. --json variants (must be valid JSON) ---------------------
JSONCHECK=0
if command -v python3 >/dev/null 2>&1; then
    JSONCHECK=1
fi
if [ "$JSONCHECK" = 1 ]; then
    for c in du top dups types ages report; do
        if "$BIN" --json "$c" "$TT/project" > "$TT/out.json" 2>/dev/null \
           && python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$TT/out.json" 2>/dev/null; then
            ok "--json $c is valid JSON"
        else
            bad "--json $c is valid JSON"
        fi
    done
    dups_json=$("$BIN" --json dups "$TT/project")
    if printf '%s' "$dups_json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["groups"] == 1, d
assert d["redundant_files"] == 2, d
assert d["wasted_bytes"] == 400, d
assert len(d["items"][0]["paths"]) == 3, d
'; then
        ok "--json dups payload matches expectations"
    else
        bad "--json dups payload matches expectations"
    fi
else
    note "python3 not available; JSON validation skipped"
fi

# ---- 11. exit codes & CLI contract --------------------------------
"$BIN" --version >/dev/null 2>&1 && ok "--version exits 0" || bad "--version exits 0"
"$BIN" --help    >/dev/null 2>&1 && ok "--help exits 0"    || bad "--help exits 0"
"$BIN" du --help >/dev/null 2>&1 && ok "du --help exits 0" || bad "du --help exits 0"
"$BIN" du "$TT/definitely-missing" >/dev/null 2>&1 && bad "missing path exits 1" || ok "missing path exits 1"
"$BIN" du --no-such-option >/dev/null 2>&1 && bad "bad option exits 2" || ok "bad option exits 2"
"$BIN" no-such-command  >/dev/null 2>&1 && bad "unknown command exits 2" || ok "unknown command exits 2"
"$BIN" >/dev/null 2>&1 && bad "no command exits 2" || ok "no command exits 2"
"$BIN" tree "$TT/project" --json >/dev/null 2>&1 && bad "tree rejects --json" || ok "tree rejects --json"

# global option before the command must behave like after it
a=$("$BIN" --si du "$TT/project" -d0 2>/dev/null | tail -1)
b=$("$BIN" du "$TT/project" -d0 --si 2>/dev/null | tail -1)
[ "$a" = "$b" ] && ok "global options work before and after command" \
                || bad "global option position matters ('$a' vs '$b')"

# ---- 12. housekeeping ---------------------------------------------
rm -rf "$TT"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
